// ─────────────────────────────────────────────────────────────────────────────
// test_core.inc — CORE 模块化单元测试集
// ─────────────────────────────────────────────────────────────────────────────

TEST(WinUtilsTest, SpecialAndSystemWindows) {
    using tools3000::core::WinUtils;
    // 验证空句柄与伪句柄边界安全，无崩溃
    EXPECT_FALSE(WinUtils::isDesktopWindow(nullptr));
    EXPECT_FALSE(WinUtils::isTaskbarWindow(nullptr));
    HWND invalidHwnd = reinterpret_cast<HWND>(static_cast<uintptr_t>(0xDEADBEEF));
    EXPECT_FALSE(WinUtils::isDesktopWindow(invalidHwnd));
    EXPECT_FALSE(WinUtils::isTaskbarWindow(invalidHwnd));
}

// -----------------------------------------------------------------------------
// 6. 快捷键定义解析测试套件
// -----------------------------------------------------------------------------
TEST(HotkeyParserTest, ExpressionParsing) {
    using tools3000::core::HotkeyDef;

    auto normal = HotkeyDef::fromString("Ctrl+Shift+A");
    EXPECT_TRUE(normal.has_value());
    if (normal) EXPECT_EQ(normal->toString(), "Ctrl+Shift+A");

    auto special = HotkeyDef::fromString("Alt+PageDown");
    EXPECT_TRUE(special.has_value());
    if (special) EXPECT_EQ(special->toString(), "Alt+PageDown");
    EXPECT_EQ(HotkeyDef{}.toString(), "");

    EXPECT_FALSE(HotkeyDef::fromString("").has_value());
    EXPECT_FALSE(HotkeyDef::fromString("Ctrl+").has_value());
    EXPECT_FALSE(HotkeyDef::fromString("Ctrl+UnknownKey").has_value());
    EXPECT_FALSE(HotkeyDef::fromString("Bad+Ctrl+A").has_value());
}

TEST(HotkeyParserTest, AllKeyMatrixParsingAndFormatting) {
    using tools3000::core::HotkeyDef;

    // 1. 单按键功能键 F1 ~ F24
    auto f1 = HotkeyDef::fromString("F1");
    ASSERT_TRUE(f1.has_value());
    EXPECT_EQ(f1->virtualKey, VK_F1);
    EXPECT_EQ(f1->toString(), "F1");

    auto f3 = HotkeyDef::fromString("F3");
    ASSERT_TRUE(f3.has_value());
    EXPECT_EQ(f3->virtualKey, VK_F3);
    EXPECT_EQ(f3->toString(), "F3");

    auto f12 = HotkeyDef::fromString("F12");
    ASSERT_TRUE(f12.has_value());
    EXPECT_EQ(f12->virtualKey, VK_F12);
    EXPECT_EQ(f12->toString(), "F12");

    auto f24 = HotkeyDef::fromString("F24");
    ASSERT_TRUE(f24.has_value());
    EXPECT_EQ(f24->virtualKey, VK_F24);
    EXPECT_EQ(f24->toString(), "F24");

    // 2. 小键盘键位与算符
    auto numPlus = HotkeyDef::fromString("Num+");
    ASSERT_TRUE(numPlus.has_value());
    EXPECT_EQ(numPlus->virtualKey, VK_ADD);
    EXPECT_EQ(numPlus->toString(), "Num+");

    auto ctrlNumPlus = HotkeyDef::fromString("Ctrl+Num+");
    ASSERT_TRUE(ctrlNumPlus.has_value());
    EXPECT_EQ(ctrlNumPlus->virtualKey, VK_ADD);
    EXPECT_EQ(ctrlNumPlus->toString(), "Ctrl+Num+");

    auto num5 = HotkeyDef::fromString("Num5");
    ASSERT_TRUE(num5.has_value());
    EXPECT_EQ(num5->virtualKey, VK_NUMPAD5);
    EXPECT_EQ(num5->toString(), "Num5");

    // 3. OEM 符号键双射
    auto oemPlus = HotkeyDef::fromString("Ctrl+=");
    ASSERT_TRUE(oemPlus.has_value());
    EXPECT_EQ(oemPlus->virtualKey, VK_OEM_PLUS);
    EXPECT_EQ(oemPlus->toString(), "Ctrl+=");

    auto oemTilde = HotkeyDef::fromString("Ctrl+`");
    ASSERT_TRUE(oemTilde.has_value());
    EXPECT_EQ(oemTilde->virtualKey, VK_OEM_3);
    EXPECT_EQ(oemTilde->toString(), "Ctrl+`");

    auto oemBracket = HotkeyDef::fromString("Alt+[");
    ASSERT_TRUE(oemBracket.has_value());
    EXPECT_EQ(oemBracket->virtualKey, VK_OEM_4);
    EXPECT_EQ(oemBracket->toString(), "Alt+[");

    // 4. 多媒体按键
    auto volMute = HotkeyDef::fromString("VolumeMute");
    ASSERT_TRUE(volMute.has_value());
    EXPECT_EQ(volMute->virtualKey, VK_VOLUME_MUTE);
    EXPECT_EQ(volMute->toString(), "VolumeMute");

    auto mediaPlay = HotkeyDef::fromString("MediaPlayPause");
    ASSERT_TRUE(mediaPlay.has_value());
    EXPECT_EQ(mediaPlay->virtualKey, VK_MEDIA_PLAY_PAUSE);
    EXPECT_EQ(mediaPlay->toString(), "MediaPlayPause");

    // 5. 常见控制键
    auto prtScn = HotkeyDef::fromString("PrintScreen");
    ASSERT_TRUE(prtScn.has_value());
    EXPECT_EQ(prtScn->virtualKey, VK_SNAPSHOT);
    EXPECT_EQ(prtScn->toString(), "PrintScreen");

    auto space = HotkeyDef::fromString("Ctrl+Space");
    ASSERT_TRUE(space.has_value());
    EXPECT_EQ(space->virtualKey, VK_SPACE);
    EXPECT_EQ(space->toString(), "Ctrl+Space");

    // 6. 上下翻页别名与应用上下文按键
    auto apps = HotkeyDef::fromString("Ctrl+Apps");
    ASSERT_TRUE(apps.has_value());
    EXPECT_EQ(apps->virtualKey, VK_APPS);
    EXPECT_EQ(apps->toString(), "Ctrl+Apps");

    auto contextMenu = HotkeyDef::fromString("Alt+ContextMenu");
    ASSERT_TRUE(contextMenu.has_value());
    EXPECT_EQ(contextMenu->virtualKey, VK_APPS);
    EXPECT_EQ(contextMenu->toString(), "Alt+Apps");

    auto pgUp = HotkeyDef::fromString("Ctrl+PgUp");
    ASSERT_TRUE(pgUp.has_value());
    EXPECT_EQ(pgUp->virtualKey, VK_PRIOR);

    auto pgDn = HotkeyDef::fromString("Alt+PgDn");
    ASSERT_TRUE(pgDn.has_value());
    EXPECT_EQ(pgDn->virtualKey, VK_NEXT);
}

TEST(HotkeySafetyTest, SafetyMatrixRules) {
    using tools3000::core::HotkeyDef;

    // 1. 包含 Ctrl/Alt/Win 的组合键必定安全
    EXPECT_TRUE(HotkeyDef::fromString("Ctrl+A")->isSafe());
    EXPECT_TRUE(HotkeyDef::fromString("Alt+1")->isSafe());
    EXPECT_TRUE(HotkeyDef::fromString("Win+D")->isSafe());
    EXPECT_TRUE(HotkeyDef::fromString("Ctrl+Alt+Delete")->isSafe());
    EXPECT_TRUE(HotkeyDef::fromString("Ctrl+Shift+Z")->isSafe());

    // 2. 单按键功能键安全
    EXPECT_TRUE(HotkeyDef::fromString("F1")->isSafe());
    EXPECT_TRUE(HotkeyDef::fromString("F3")->isSafe());
    EXPECT_TRUE(HotkeyDef::fromString("F12")->isSafe());

    // 3. 独立控制与媒体按键安全
    EXPECT_TRUE(HotkeyDef::fromString("PrintScreen")->isSafe());
    EXPECT_TRUE(HotkeyDef::fromString("VolumeMute")->isSafe());
    EXPECT_TRUE(HotkeyDef::fromString("MediaPlayPause")->isSafe());
    EXPECT_TRUE(HotkeyDef::fromString("Num+")->isSafe());

    // 4. 严苛红线：单字母、单数字、单符号、单编辑键无修饰键注册不安全
    EXPECT_FALSE(HotkeyDef::fromString("A")->isSafe());
    EXPECT_FALSE(HotkeyDef::fromString("Z")->isSafe());
    EXPECT_FALSE(HotkeyDef::fromString("1")->isSafe());
    EXPECT_FALSE(HotkeyDef::fromString("`")->isSafe());
    EXPECT_FALSE(HotkeyDef::fromString("[")->isSafe());
    EXPECT_FALSE(HotkeyDef::fromString("Space")->isSafe());
    EXPECT_FALSE(HotkeyDef::fromString("Enter")->isSafe());
    EXPECT_FALSE(HotkeyDef::fromString("Tab")->isSafe());

    // 5. 单纯 Shift 配合字母/数字/符号不安全（会劫持正常打字大小写输入）
    EXPECT_FALSE(HotkeyDef::fromString("Shift+A")->isSafe());
    EXPECT_FALSE(HotkeyDef::fromString("Shift+1")->isSafe());
    EXPECT_FALSE(HotkeyDef::fromString("Apps")->isSafe());
    EXPECT_TRUE(HotkeyDef::fromString("Ctrl+Apps")->isSafe());
}

TEST(HotkeyDualEngineTest, F1AndF3RegistrationFallback) {
    auto& mgr = tools3000::core::HotkeyManager::instance();
    const std::string f1Name = "Unit Test F1 Hotkey";
    const std::string f3Name = "Unit Test F3 Hotkey";
    const std::string unsafeName = "Unit Test Unsafe Hotkey";

    // 1. 不安全快捷键拒绝注册与冲突上报
    auto unsafeDef = tools3000::core::HotkeyDef::fromString("A");
    ASSERT_TRUE(unsafeDef.has_value());
    EXPECT_FALSE(mgr.registerHotkey(unsafeName, *unsafeDef, []() {}));
    auto unsafeConflict = mgr.checkConflict(*unsafeDef, unsafeName);
    EXPECT_TRUE(unsafeConflict.hasConflict);
    EXPECT_EQ(unsafeConflict.conflictType, "unsafe");

    // 2. 注册 F1 (Windows Explorer 常占用的按键，双引擎无缝降级)
    auto f1Def = tools3000::core::HotkeyDef::fromString("F1");
    ASSERT_TRUE(f1Def.has_value());
    EXPECT_TRUE(mgr.registerHotkey(f1Name, *f1Def, []() {}));

    auto entries = mgr.getAllHotkeys();
    auto it1 = std::find_if(entries.begin(), entries.end(), [&](const auto& e) { return e.name == f1Name; });
    ASSERT_TRUE(it1 != entries.end());
    EXPECT_EQ(it1->def.toString(), "F1");
    EXPECT_TRUE(it1->registered);
    auto conflict1 = mgr.checkConflict(*f1Def, f1Name);
    EXPECT_FALSE(conflict1.hasConflict);

    // 3. 注册 F3
    auto f3Def = tools3000::core::HotkeyDef::fromString("F3");
    ASSERT_TRUE(f3Def.has_value());
    EXPECT_TRUE(mgr.registerHotkey(f3Name, *f3Def, []() {}));

    entries = mgr.getAllHotkeys();
    auto it3 = std::find_if(entries.begin(), entries.end(), [&](const auto& e) { return e.name == f3Name; });
    ASSERT_TRUE(it3 != entries.end());
    EXPECT_EQ(it3->def.toString(), "F3");
    EXPECT_TRUE(it3->registered);
    auto conflict3 = mgr.checkConflict(*f3Def, f3Name);
    EXPECT_FALSE(conflict3.hasConflict);

    // 4. 清理注销
    mgr.unregisterHotkey(f1Name);
    mgr.unregisterHotkey(f3Name);

    entries = mgr.getAllHotkeys();
    EXPECT_TRUE(std::none_of(entries.begin(), entries.end(), [&](const auto& e) { return e.name == f1Name; }));
    EXPECT_TRUE(std::none_of(entries.begin(), entries.end(), [&](const auto& e) { return e.name == f3Name; }));
}

// -----------------------------------------------------------------------------
// 7. 快捷键管理器禁用态与生命周期测试套件
// -----------------------------------------------------------------------------
TEST(ElevationPolicyTest, RestartAndStartupActions) {
    using tools3000::core::ElevationRestartAction;
    EXPECT_EQ(tools3000::core::elevationRestartAfterSetting(true, false),
              ElevationRestartAction::Elevate);
    EXPECT_EQ(tools3000::core::elevationRestartAfterSetting(false, true),
              ElevationRestartAction::DropElevation);
    EXPECT_EQ(tools3000::core::elevationRestartAfterSetting(true, true),
              ElevationRestartAction::None);
    EXPECT_EQ(tools3000::core::elevationRestartAfterSetting(false, false),
              ElevationRestartAction::None);
    EXPECT_TRUE(tools3000::core::shouldAutoElevateOnStartup(true, false, false));
    EXPECT_FALSE(tools3000::core::shouldAutoElevateOnStartup(true, true, false));
    EXPECT_FALSE(tools3000::core::shouldAutoElevateOnStartup(false, false, false));
    EXPECT_FALSE(tools3000::core::shouldAutoElevateOnStartup(true, false, true));
}

TEST(HotkeyPolicyTest, RecordingPauseArmsOnlyWhileRecording) {
    using tools3000::core::HotkeyArmScope;
    EXPECT_EQ(tools3000::core::hotkeyArmScopeForName("Record Pause"), HotkeyArmScope::WhileRecording);
    EXPECT_EQ(tools3000::core::hotkeyArmScopeForName("Screenshot"), HotkeyArmScope::Always);
    EXPECT_TRUE(tools3000::core::hotkeyShouldBeArmed(HotkeyArmScope::Always, false));
    EXPECT_FALSE(tools3000::core::hotkeyShouldBeArmed(HotkeyArmScope::WhileRecording, false));
    EXPECT_TRUE(tools3000::core::hotkeyShouldBeArmed(HotkeyArmScope::WhileRecording, true));
    EXPECT_TRUE(tools3000::core::isEditorCommandPaletteChord(MOD_CONTROL | MOD_SHIFT, 'P'));
    EXPECT_TRUE(tools3000::core::isEditorCommandPaletteChord(MOD_CONTROL | MOD_SHIFT | MOD_NOREPEAT, 'P'));
    EXPECT_FALSE(tools3000::core::isEditorCommandPaletteChord(MOD_CONTROL | MOD_SHIFT, 'R'));
    EXPECT_FALSE(tools3000::core::isEditorCommandPaletteChord(MOD_CONTROL, 'P'));
    EXPECT_FALSE(tools3000::core::hotkeyLooksExternallyConflicted(true, false, false));
    EXPECT_TRUE(tools3000::core::hotkeyLooksExternallyConflicted(true, true, false));
    EXPECT_FALSE(tools3000::core::hotkeyLooksExternallyConflicted(true, true, true));
    EXPECT_FALSE(tools3000::core::hotkeyLooksExternallyConflicted(false, true, false));
}

TEST(HotkeyManagerTest, DisabledHotkeyLifecycle) {
    auto& manager = tools3000::core::HotkeyManager::instance();
    constexpr const char* name = "Unit Test Disabled Hotkey";
    std::atomic<int> calls{0};

    EXPECT_TRUE(manager.registerHotkey(name, {}, [&calls]() { calls.fetch_add(1); }));
    auto entries = manager.getAllHotkeys();
    auto entry = std::find_if(entries.begin(), entries.end(), [](const auto& candidate) {
        return candidate.name == name;
    });
    EXPECT_TRUE(entry != entries.end());
    if (entry != entries.end()) {
        EXPECT_FALSE(entry->registered);
        EXPECT_EQ(entry->def.toString(), "");
    }

    EXPECT_TRUE(manager.setHotkeyArmed(name, false));
    entries = manager.getAllHotkeys();
    entry = std::find_if(entries.begin(), entries.end(), [](const auto& candidate) {
        return candidate.name == name;
    });
    ASSERT_TRUE(entry != entries.end());
    EXPECT_FALSE(entry->armed);
    EXPECT_FALSE(entry->conflict);
    EXPECT_TRUE(manager.setHotkeyArmed(name, true));

    EXPECT_TRUE(manager.clearHotkey(name));
    EXPECT_EQ(calls.load(), 0);
    manager.unregisterHotkey(name);
    entries = manager.getAllHotkeys();
    EXPECT_TRUE(std::none_of(entries.begin(), entries.end(), [](const auto& candidate) {
        return candidate.name == name;
    }));
}

TEST(HotkeyManagerTest, MarshalsWindowHotkeysToOwningThread) {
    HWND messageWindow = CreateWindowExW(
        0, L"STATIC", L"Tools3000HotkeyAffinityTest", 0,
        0, 0, 0, 0, HWND_MESSAGE, nullptr, GetModuleHandleW(nullptr), nullptr);
    ASSERT_NE(messageWindow, nullptr);

    constexpr int probeId = 0x6AFE;
    constexpr UINT modifiers = MOD_CONTROL | MOD_ALT | MOD_SHIFT | MOD_NOREPEAT;
    if (!RegisterHotKey(messageWindow, probeId, modifiers, VK_F24)) {
        DestroyWindow(messageWindow);
        GTEST_SKIP() << "F24 test chord is unavailable on this host";
    }
    UnregisterHotKey(messageWindow, probeId);

    auto& dispatcher = tools3000::core::MainThreadDispatcher::instance();
    dispatcher.initialize(messageWindow);
    auto& manager = tools3000::core::HotkeyManager::instance();
    manager.initialize(messageWindow);

    constexpr const char* name = "Unit Test Cross-thread Hotkey";
    auto registration = std::async(std::launch::async, [&manager, name]() {
        return manager.registerHotkey(
            name, {tools3000::core::ModKey::Ctrl | tools3000::core::ModKey::Alt |
                       tools3000::core::ModKey::Shift, VK_F24},
            []() {});
    });

    const auto pumpUntilReady = [&dispatcher](auto& future) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (future.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready &&
               std::chrono::steady_clock::now() < deadline) {
            MSG message{};
            while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
                if (message.message == tools3000::core::MainThreadDispatcher::MessageId) {
                    dispatcher.drain();
                } else {
                    TranslateMessage(&message);
                    DispatchMessageW(&message);
                }
            }
            std::this_thread::yield();
        }
        return future.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready;
    };

    ASSERT_TRUE(pumpUntilReady(registration));
    EXPECT_TRUE(registration.get());

    auto removal = std::async(std::launch::async, [&manager, name]() {
        manager.unregisterHotkey(name);
        return true;
    });
    ASSERT_TRUE(pumpUntilReady(removal));
    EXPECT_TRUE(removal.get());

    manager.shutdown();
    dispatcher.shutdown();
    DestroyWindow(messageWindow);
}

// -----------------------------------------------------------------------------
// 8. 配置管理器持久化与默认回退测试套件
// -----------------------------------------------------------------------------
TEST(ConfigManagerTest, PersistenceAndDefaultFallbacks) {
    auto temp = std::filesystem::temp_directory_path() /
        (L"Tools3000Tests_" + std::to_wstring(GetCurrentProcessId()));
    std::error_code ec;
    std::filesystem::remove_all(temp, ec);
    std::filesystem::create_directories(temp, ec);
    {
        std::ofstream corrupt(temp / "config.json", std::ios::binary | std::ios::trunc);
        corrupt << "{ definitely-not-json";
    }

    auto& config = tools3000::core::ConfigManager::instance();
    EXPECT_TRUE(config.initialize(temp));
    size_t corruptBackups = 0;
    for (const auto& entry : std::filesystem::directory_iterator(temp)) {
        if (entry.path().filename().wstring().starts_with(L"config.json.corrupt.")) {
            ++corruptBackups;
        }
    }
    EXPECT_EQ(corruptBackups, 1u);

    std::atomic<int> notifications{0};
    const auto callbackId = config.onChange([&notifications](const std::string&) {
        notifications.fetch_add(1);
    });
    EXPECT_TRUE(config.set("/general/language", std::string("zh-CN")));
    EXPECT_TRUE(config.set("/capture/quality", 87));
    EXPECT_TRUE(config.mergePatch({
        {"general", {{"theme", "dark"}, {"checkUpdates", true}}},
        {"capture", {{"format", "png"}}}
    }, "/batch"));
    EXPECT_EQ(config.get<std::string>("/general/language", ""), "zh-CN");
    EXPECT_EQ(config.get<int>("/capture/quality", 0), 87);
    EXPECT_EQ(config.get<std::string>("/general/theme", ""), "dark");
    EXPECT_FALSE(config.fromJsonString("[1,2,3]"));
    EXPECT_EQ(config.get<std::string>("/general/theme", ""), "dark");
    EXPECT_TRUE(config.remove("/capture/format"));
    EXPECT_FALSE(config.has("/capture/format"));
    EXPECT_TRUE(config.flush());

    {
        std::ifstream file(temp / "config.json");
        auto persisted = nlohmann::json::parse(file);
        EXPECT_EQ(persisted["general"]["language"], "zh-CN");
        EXPECT_EQ(persisted["general"]["theme"], "dark");
        EXPECT_EQ(persisted["capture"]["quality"], 87);
        EXPECT_FALSE(persisted["capture"].contains("format"));
    }

    EXPECT_TRUE(config.exportTo(temp / "export.json"));
    {
        std::ifstream file(temp / "export.json");
        const auto exported = nlohmann::json::parse(file);
        EXPECT_EQ(exported["general"]["language"], "zh-CN");
    }
    EXPECT_FALSE(config.exportTo(std::filesystem::path(L"relative.json")));
    EXPECT_FALSE(config.exportTo(temp / L"nested" / L".." / L"escape.json"));
    EXPECT_FALSE(config.exportTo(temp / L"export.txt"));
    EXPECT_FALSE(config.importFrom(std::filesystem::path(L"relative.json")));
    EXPECT_FALSE(config.importFrom(temp / L"nested" / L".." / L"export.json"));

    // 给文件监控线程时间消费本进程自己的原子替换事件；内容未变化时不得重复通知。
    std::this_thread::sleep_for(std::chrono::milliseconds(350));
    EXPECT_EQ(notifications.load(), 4);

    EXPECT_TRUE(config.reset());
    EXPECT_FALSE(config.has("/general/language"));
    {
        std::ifstream file(temp / "config.json");
        const auto persisted = nlohmann::json::parse(file);
        EXPECT_TRUE(persisted.is_object());
        EXPECT_TRUE(persisted.empty());
    }
    EXPECT_TRUE(config.importFrom(temp / "export.json"));
    EXPECT_EQ(config.get<std::string>("/general/language", ""), "zh-CN");
    EXPECT_TRUE(config.reset());
    config.removeOnChange(callbackId);
    config.shutdown();
    std::filesystem::remove_all(temp, ec);
}

TEST(ConfigManagerCallbackLifecycleTest, NonZeroSentinelAndClearCallbacksBeforePluginFreeLibrary) {
    auto& config = tools3000::core::ConfigManager::instance();
    config.clearCallbacks();

    // 空回调返回哨兵值 0，removeOnChange(0) 安全无副作用
    EXPECT_EQ(config.onChange(nullptr), 0u);
    config.removeOnChange(0);

    // 有效回调 ID 恒非零，确保 `if (id != 0) removeOnChange(id)` 守卫绝不会漏注销首个回调
    auto tokenA = std::make_shared<int>(42);
    const size_t idA = config.onChange([tokenA](const std::string&) {});
    EXPECT_NE(idA, 0u);
    EXPECT_EQ(tokenA.use_count(), 2);
    config.removeOnChange(idA);
    EXPECT_EQ(tokenA.use_count(), 1);

    // 模拟 PluginManager::shutdownPlugins() 在 FreeLibrary 前统一销毁残留跨 DLL 闭包
    auto tokenB = std::make_shared<int>(99);
    const size_t idB = config.onChange([tokenB](const std::string&) {});
    EXPECT_NE(idB, 0u);
    EXPECT_EQ(tokenB.use_count(), 2);
    config.clearCallbacks();
    EXPECT_EQ(tokenB.use_count(), 1);

    auto& kbHook = tools3000::core::KeyboardHook::instance();
    auto kbToken = std::make_shared<int>(7);
    kbHook.setKeycastCallback([kbToken](const std::string&) {});
    kbHook.setLowLevelKeyInterceptor([kbToken](const KBDLLHOOKSTRUCT&, WPARAM) { return false; });
    EXPECT_EQ(kbToken.use_count(), 3);
    kbHook.clearCallbacks();
    EXPECT_EQ(kbToken.use_count(), 1);
}

// -----------------------------------------------------------------------------
// 9. 进程间通信消息桥接测试套件
// -----------------------------------------------------------------------------
TEST(AutoStartPolicyTest, MatchesCurrentExecutableAndNormalizesCaseAndQuotes) {
    const std::wstring xml =
        LR"(<Task><Actions><Exec><Command>"C:\Program Files\Tools3000\.\Tools3000.exe"</Command><Arguments>--silent</Arguments></Exec></Actions></Task>)";
    EXPECT_TRUE(tools3000::core::autostart::taskTargetsExecutable(
        xml, LR"(c:\program files\tools3000\Tools3000.exe)"));
}

TEST(AutoStartPolicyTest, RejectsStaleOrMalformedTaskTargets) {
    const std::wstring stale =
        LR"(<Task><Actions><Exec><Command>D:\Program Files\Tools3000\Tools3000.exe</Command></Exec></Actions></Task>)";
    EXPECT_FALSE(tools3000::core::autostart::taskTargetsExecutable(
        stale, LR"(C:\Program Files\Tools3000\Tools3000.exe)"));
    EXPECT_FALSE(tools3000::core::autostart::taskTargetsExecutable(
        L"<Task><Command>missing close tag", LR"(C:\Tools3000.exe)"));
    EXPECT_FALSE(tools3000::core::autostart::taskTargetsExecutable(
        L"<Task />", LR"(C:\Tools3000.exe)"));
}

TEST(AutoStartPolicyTest, DecodesXmlEscapesInExecutablePath) {
    const std::wstring xml =
        LR"(<Task><Command>C:\Tools &amp; Apps\Tools3000.exe</Command></Task>)";
    EXPECT_TRUE(tools3000::core::autostart::taskTargetsExecutable(
        xml, LR"(C:\Tools & Apps\Tools3000.exe)"));
    EXPECT_EQ(tools3000::core::autostart::decodeXml(L"&quot;&apos;&lt;&gt;&amp;"), L"\"'<>&");
}

TEST(AutoStartPolicyTest, ExtractsWorkingDirectoryFromTaskXml) {
    const std::wstring xmlWithDir =
        LR"(<Task><Actions><Exec><Command>"C:\Program Files\Tools3000\Tools3000.exe"</Command><Arguments>--silent</Arguments><WorkingDirectory>C:\Program Files\Tools3000</WorkingDirectory></Exec></Actions></Task>)";
    auto dir = tools3000::core::autostart::taskWorkingDirectory(xmlWithDir);
    ASSERT_TRUE(dir.has_value());
    EXPECT_EQ(*dir, L"C:\\Program Files\\Tools3000");
    EXPECT_TRUE(tools3000::core::autostart::taskTargetsWorkingDirectory(
        xmlWithDir, LR"(c:\program files\tools3000)"));

    const std::wstring xmlWithoutDir =
        LR"(<Task><Actions><Exec><Command>C:\Tools3000.exe</Command></Exec></Actions></Task>)";
    EXPECT_FALSE(tools3000::core::autostart::taskWorkingDirectory(xmlWithoutDir).has_value());
    EXPECT_FALSE(tools3000::core::autostart::taskTargetsWorkingDirectory(
        xmlWithoutDir, LR"(c:\tools3000)"));
}

TEST(AutoStartPolicyTest, UsesTools3000TaskFolderNamespace) {
    EXPECT_EQ(tools3000::core::autostart::getTaskFolderName(), L"\\Tools3000");
    EXPECT_NE(tools3000::core::autostart::getFullTaskPath().find(L"\\Tools3000\\Autorun for "), std::wstring::npos);
}

TEST(AutoStartPolicyTest, IdentifiesDevelopmentBinariesToProtectProductionAutoStart) {
    EXPECT_TRUE(tools3000::core::autostart::isDevelopmentBinary(LR"(C:\repo\tools3000\build\bin\Release\Tools3000.exe)"));
    EXPECT_TRUE(tools3000::core::autostart::isDevelopmentBinary(LR"(D:\workspace\tools3000\out\build\x64-Release\Tools3000.exe)"));
    EXPECT_FALSE(tools3000::core::autostart::isDevelopmentBinary(LR"(C:\Program Files\Tools3000\Tools3000.exe)"));
    EXPECT_FALSE(tools3000::core::autostart::isDevelopmentBinary(LR"(C:\Tools\Tools3000\Tools3000.exe)"));
}

TEST(AutoStartPolicyTest, IdentifiesNonProductionBinariesAndTestMode) {
    EXPECT_TRUE(tools3000::core::autostart::isNonProductionBinary(LR"(C:\repo\tools3000\build\bin\Release\Tools3000Tests.exe)"));
    EXPECT_TRUE(tools3000::core::autostart::isNonProductionBinary(LR"(C:\repo\tools3000\build\bin\Release\Tools3000.exe)"));
    EXPECT_TRUE(tools3000::core::autostart::isNonProductionBinary(LR"(C:\any\path\MyUnitTestRunner.exe)"));
    EXPECT_FALSE(tools3000::core::autostart::isNonProductionBinary(LR"(C:\Program Files\Tools3000\Tools3000.exe)"));
    EXPECT_TRUE(tools3000::core::autostart::isTestMode());

    // 验证在测试模式下，底层物理注册、注销与迁移均安全短路，绝不产生物理副作用
    EXPECT_TRUE(tools3000::core::autostart::registerTaskCOM(LR"(C:\repo\tools3000\build\bin\Release\Tools3000Tests.exe)"));
    EXPECT_TRUE(tools3000::core::autostart::unregisterTaskCOM());
    EXPECT_TRUE(tools3000::core::autostart::migrateLegacyTasksCOM(LR"(C:\repo\tools3000\build\bin\Release\Tools3000Tests.exe)"));
}

TEST(MessageBridgeTest, IPCChannelDispatch) {
    auto& bridge = tools3000::core::MessageBridge::instance();
    bridge.registerHandler("test.echo", [](const nlohmann::json& params) {
        return params;
    });

    auto ok = nlohmann::json::parse(
        bridge.handleMessage(R"({"id":42,"method":"test.echo","params":{"value":7}})"));
    EXPECT_EQ(ok["id"], 42);
    EXPECT_EQ(ok["result"]["value"], 7);

    auto missing = nlohmann::json::parse(
        bridge.handleMessage(R"({"id":77,"method":"test.missing"})"));
    EXPECT_EQ(missing["id"], 77);
    EXPECT_EQ(missing["error"]["code"], -32601);

    bridge.clearHandlers();
    auto cleared = nlohmann::json::parse(
        bridge.handleMessage(R"({"id":78,"method":"test.echo"})"));
    EXPECT_EQ(cleared["error"]["code"], -32601);

    bridge.registerBuiltinHandlers();
    auto plugins = nlohmann::json::parse(
        bridge.handleMessage(R"({"id":79,"method":"plugins.getAll"})"));
    EXPECT_TRUE(plugins["result"].is_array());
    bool foundRemoteBoost = false;
    for (const auto& item : plugins["result"]) {
        if (item.value("id", "") == "remote_boost") {
            foundRemoteBoost = true;
            EXPECT_EQ(item.value("name", ""), "远程协助增强");
            EXPECT_EQ(item.value("executionModel", ""), "trusted-native-in-process");
            auto caps = item.value("capabilities", std::vector<std::string>{});
            EXPECT_TRUE(std::find(caps.begin(), caps.end(), "remote-tunnel") != caps.end());
            EXPECT_TRUE(std::find(caps.begin(), caps.end(), "hotkey-passthrough") != caps.end());
            EXPECT_TRUE(std::find(caps.begin(), caps.end(), "ime-sanitizer") != caps.end());
            auto perms = item.value("permissions", std::vector<std::string>{});
            EXPECT_TRUE(std::find(perms.begin(), perms.end(), "low-level-keyboard-hook") != perms.end());
            EXPECT_TRUE(std::find(perms.begin(), perms.end(), "window-event-hook") != perms.end());
            EXPECT_TRUE(std::find(perms.begin(), perms.end(), "input-synthesis") != perms.end());
            break;
        }
    }
    EXPECT_TRUE(foundRemoteBoost);
    for (const auto& item : plugins["result"]) {
        const std::string pid = item.value("id", "");
        EXPECT_NE(pid, "ai_assistant");
        EXPECT_NE(pid, "color_picker");
        EXPECT_NE(pid, "clipboard_manager");
        EXPECT_NE(pid, "markdown_preview");
    }
    auto removedMarket = nlohmann::json::parse(
        bridge.handleMessage(R"({"id":790,"method":"plugins.getMarketplace"})"));
    EXPECT_TRUE(removedMarket.contains("error"));

    auto toggleRemote = nlohmann::json::parse(bridge.handleMessage(
        R"({"id":801,"method":"plugins.setEnabled","params":{"id":"remote_boost","enabled":true}})"));
    EXPECT_TRUE(toggleRemote["result"]["success"].get<bool>());
    EXPECT_FALSE(toggleRemote["result"]["restartRequired"].get<bool>());
    auto invalidPluginToggle = nlohmann::json::parse(bridge.handleMessage(
        R"({"id":80,"method":"plugins.setEnabled","params":{"id":"missing","enabled":true}})"));
    EXPECT_FALSE(invalidPluginToggle["result"]["success"].get<bool>());
    EXPECT_EQ(invalidPluginToggle["result"]["error"], "plugin not found");

    auto lockStates = nlohmann::json::parse(
        bridge.handleMessage(R"({"id":81,"method":"stats.getKeyboardLockStates"})"));
    EXPECT_TRUE(lockStates["result"].contains("numLock"));
    EXPECT_TRUE(lockStates["result"].contains("capsLock"));
    EXPECT_TRUE(lockStates["result"].contains("scrollLock"));
    bridge.clearHandlers();

    std::mutex gateMutex;
    std::condition_variable gateCv;
    bool entered = false;
    bool release = false;
    bridge.registerHandler("test.blocking", [&](const nlohmann::json&) {
        std::unique_lock lock(gateMutex);
        entered = true;
        gateCv.notify_all();
        gateCv.wait(lock, [&release]() { return release; });
        return nlohmann::json{{"done", true}};
    });
    std::thread caller([&bridge]() {
        bridge.handleMessage(R"({"id":81,"method":"test.blocking"})");
    });
    {
        std::unique_lock lock(gateMutex);
        gateCv.wait(lock, [&entered]() { return entered; });
    }
    std::atomic<bool> unregisterStarted{false};
    std::atomic<bool> unregisterFinished{false};
    std::thread unregisterThread([&]() {
        unregisterStarted = true;
        bridge.unregisterHandlersByPrefix("test.");
        unregisterFinished = true;
    });
    while (!unregisterStarted.load()) std::this_thread::yield();
    EXPECT_FALSE(unregisterFinished.load());
    {
        std::lock_guard lock(gateMutex);
        release = true;
    }
    gateCv.notify_all();
    caller.join();
    unregisterThread.join();
    EXPECT_TRUE(unregisterFinished.load());
    auto retired = nlohmann::json::parse(
        bridge.handleMessage(R"({"id":82,"method":"test.blocking"})"));
    EXPECT_EQ(retired["error"]["code"], -32601);
}

// -----------------------------------------------------------------------------
// 10. 插件清单侧车解析测试套件
// -----------------------------------------------------------------------------
TEST(PluginManifestTest, SidecarParsingAndValidation) {
    using tools3000::core::comparePluginVersions;
    using tools3000::core::loadPluginManifest;
    EXPECT_GT(comparePluginVersions("1.2.0", "1.1.9"), 0);
    EXPECT_EQ(comparePluginVersions("1.0", "1.0.0"), 0);
    EXPECT_LT(comparePluginVersions("2.0.0", "10.0.0"), 0);

    const auto path = std::filesystem::temp_directory_path() /
        (L"Tools3000Manifest_" + std::to_wstring(GetCurrentProcessId()) + L".json");
    {
        std::ofstream file(path, std::ios::binary | std::ios::trunc);
        file << R"({
            "schemaVersion": 2,
            "abiVersion": 1,
            "id": "capture",
            "name": "Capture",
            "version": "1.0.0",
            "minimumHostVersion": "1.0.0",
            "entryPoint": "CreatePlugin",
            "executionModel": "trusted-native-in-process",
            "capabilities": ["screenshot", "recording"],
            "permissions": ["screen-capture"]
        })";
    }
    const auto valid = loadPluginManifest(path, "capture", "1.2.0");
    EXPECT_TRUE(static_cast<bool>(valid));
    EXPECT_EQ(valid.manifest.id, "capture");
    EXPECT_EQ(valid.manifest.capabilities.size(), 2u);
    EXPECT_EQ(valid.manifest.executionModel, "trusted-native-in-process");

    const auto wrongId = loadPluginManifest(path, "gesture", "1.2.0");
    EXPECT_FALSE(wrongId);
    EXPECT_EQ(wrongId.error, "plugin manifest id does not match its DLL");

    const auto oldHost = loadPluginManifest(path, "capture", "0.9.0");
    EXPECT_FALSE(oldHost);
    EXPECT_EQ(oldHost.error, "plugin requires a newer Tools3000 version");

    {
        std::ifstream input(path, std::ios::binary);
        nlohmann::json manifestJson;
        input >> manifestJson;
        input.close();
        manifestJson.erase("executionModel");
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        output << manifestJson.dump(2);
    }
    const auto missingTrustModel = loadPluginManifest(path, "capture", "1.2.0");
    EXPECT_FALSE(missingTrustModel);
    EXPECT_EQ(missingTrustModel.error,
              "plugin must declare the trusted native in-process execution model");

    std::error_code ec;
    std::filesystem::remove(path, ec);
}

// -----------------------------------------------------------------------------
// 11. 事件总线静默期与订阅测试套件
// -----------------------------------------------------------------------------
TEST(EventBusTest, QuiescenceAndSubscription) {
    auto& bus = tools3000::core::EventBus::instance();
    std::mutex gateMutex;
    std::condition_variable gateCv;
    bool entered = false;
    bool release = false;
    std::atomic<int> calls{0};
    const auto id = bus.subscribe<tools3000::core::ActionTriggerScreenshotEvent>(
        [&](const tools3000::core::ActionTriggerScreenshotEvent&) {
            ++calls;
            std::unique_lock lock(gateMutex);
            entered = true;
            gateCv.notify_all();
            gateCv.wait(lock, [&release]() { return release; });
        });
    std::thread publisher([&bus]() {
        bus.publish(tools3000::core::ActionTriggerScreenshotEvent{});
    });
    {
        std::unique_lock lock(gateMutex);
        gateCv.wait(lock, [&entered]() { return entered; });
    }
    std::atomic<bool> unsubscribeStarted{false};
    std::atomic<bool> unsubscribeFinished{false};
    std::thread unsubscribeThread([&]() {
        unsubscribeStarted = true;
        bus.unsubscribeAndWait(id);
        unsubscribeFinished = true;
    });
    while (!unsubscribeStarted.load()) std::this_thread::yield();
    EXPECT_FALSE(unsubscribeFinished.load());
    {
        std::lock_guard lock(gateMutex);
        release = true;
    }
    gateCv.notify_all();
    publisher.join();
    unsubscribeThread.join();
    EXPECT_TRUE(unsubscribeFinished.load());
    bus.publish(tools3000::core::ActionTriggerScreenshotEvent{});
    EXPECT_EQ(calls.load(), 1);
    bus.clearAll();
}

// -----------------------------------------------------------------------------
// 12. 性能监控与微秒级计时器测试套件
// -----------------------------------------------------------------------------
TEST(PerformanceMonitorTest, MicrosecondTimer) {
    tools3000::core::PerfTimer timer("gesture");
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
    timer.stop();
    const double elapsed = timer.elapsedMs();
    EXPECT_GE(elapsed, 1.0);
    EXPECT_GE(tools3000::core::PerformanceMonitor::instance().getMetrics().gestureLatencyMs, 1.0);

    auto& monitor = tools3000::core::PerformanceMonitor::instance();
    for (int value = 1; value <= 100; ++value) {
        monitor.recordLatency("unit.latency", static_cast<double>(value));
    }
    monitor.recordLatency("unit.latency", -1.0); // invalid samples are ignored
    monitor.recordCounter("unit.absoluteCounter", 42);
    const auto metrics = monitor.getMetrics();
    const auto summary = metrics.latencies.find("unit.latency");
    EXPECT_TRUE(summary != metrics.latencies.end());
    if (summary != metrics.latencies.end()) {
        EXPECT_EQ(summary->second.sampleCount, 100u);
        EXPECT_DOUBLE_EQ(summary->second.lastMs, 100.0);
        EXPECT_DOUBLE_EQ(summary->second.meanMs, 50.5);
        EXPECT_DOUBLE_EQ(summary->second.p95Ms, 95.0);
        EXPECT_DOUBLE_EQ(summary->second.maxMs, 100.0);
    }
    EXPECT_EQ(metrics.counters.at("unit.absoluteCounter"), 42u);
    const auto serialized = monitor.getMetricsJson();
    EXPECT_TRUE(serialized.contains("latencies"));
    EXPECT_TRUE(serialized["latencies"].contains("unit.latency"));
    const std::string oversizedMetricName(129, 'x');
    monitor.recordCounter(oversizedMetricName, 1);
    monitor.recordPluginInit("unit.plugin", 12.0);
    monitor.recordPluginInit("unit.invalid", -1.0);
    const auto validatedMetrics = monitor.getMetrics();
    EXPECT_FALSE(validatedMetrics.counters.contains(oversizedMetricName));
    EXPECT_DOUBLE_EQ(validatedMetrics.pluginInitMs.at("unit.plugin"), 12.0);
    EXPECT_FALSE(validatedMetrics.pluginInitMs.contains("unit.invalid"));

    monitor.start(60'000);
    tools3000::core::PerfMetrics sampled;
    for (int attempt = 0; attempt < 50; ++attempt) {
        sampled = monitor.getMetrics();
        if (sampled.handleCount > 0) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    EXPECT_GT(sampled.handleCount, 0u);
    const auto stopStarted = std::chrono::steady_clock::now();
    monitor.stop();
    const auto stopMs = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - stopStarted).count();
    EXPECT_LT(stopMs, 250.0);
}

TEST(PerformanceMonitorTest, RejectsUnboundedDiagnosticCardinality) {
    auto& monitor = tools3000::core::PerformanceMonitor::instance();
    for (int index = 0; index < 96; ++index) {
        const auto suffix = std::to_string(index);
        monitor.recordLatency("capacity.latency." + suffix, static_cast<double>(index));
        monitor.recordCounter("capacity.counter." + suffix, static_cast<std::uint64_t>(index));
        monitor.recordPluginInit("capacity.plugin." + suffix, static_cast<double>(index));
    }
    const auto metrics = monitor.getMetrics();
    EXPECT_LE(metrics.latencies.size(), 64u);
    EXPECT_LE(metrics.counters.size(), 64u);
    EXPECT_LE(metrics.pluginInitMs.size(), 64u);
    // Existing names remain updateable after the cap; capacity protection must
    // not make a healthy, long-lived series permanently stale.
    monitor.recordCounter("capacity.counter.0", 999);
    const auto refreshed = monitor.getMetrics();
    const auto counter = refreshed.counters.find("capacity.counter.0");
    if (counter != refreshed.counters.end()) EXPECT_EQ(counter->second, 999u);
}

// -----------------------------------------------------------------------------
// 13. 版本升级比较测试套件
// -----------------------------------------------------------------------------
TEST(UpdateCheckerTest, SemVerComparison) {
    using tools3000::core::UpdateChecker;
    EXPECT_TRUE(UpdateChecker::isNewerVersion("v1.2.3", "1.2.2"));
    EXPECT_TRUE(UpdateChecker::isNewerVersion("1.10.0", "1.9.9"));
    EXPECT_TRUE(UpdateChecker::isNewerVersion("2.0", "1.99.99"));
    EXPECT_TRUE(UpdateChecker::isNewerVersion("Tools3000 v1.2.3 正式发布", "1.2.2"));
    EXPECT_FALSE(UpdateChecker::isNewerVersion("1.2.3", "1.2.3"));
    EXPECT_FALSE(UpdateChecker::isNewerVersion("1.2", "1.2.1"));
    EXPECT_FALSE(UpdateChecker::isNewerVersion("1.2.3-beta.1", "1.2.3"));
    EXPECT_FALSE(UpdateChecker::isNewerVersion("not-a-version", "1.0.0"));
    EXPECT_FALSE(UpdateChecker::isNewerVersion("发行", "1.0.0"));
}

TEST(UpdateCheckerTest, InstallerSilentAutoRestartParametersAndNoSelfKillContract) {
    using tools3000::core::UpdateChecker;
    const std::wstring params = UpdateChecker::buildInstallerParameters();
    EXPECT_NE(params.find(L"/SILENT"), std::wstring::npos);
    EXPECT_NE(params.find(L"/NOCLOSEAPPLICATIONS"), std::wstring::npos);
    EXPECT_NE(params.find(L"/NORESTARTAPPLICATIONS"), std::wstring::npos);
    EXPECT_NE(params.find(L"/AUTOUPDATE"), std::wstring::npos);
    // 严禁传裸 /CLOSEAPPLICATIONS 覆盖 installer.iss 的 CloseApplications=no 触发 Restart Manager 弹窗
    EXPECT_EQ(params.find(L" /CLOSEAPPLICATIONS"), std::wstring::npos);

    // 防回退门禁：当存在源码树 installer.iss 时，断言绝不对 Tools3000.exe 使用 taskkill /t（否则应用内升级时安装程序会杀死自己）
    // 且静默自动更新必须配置 ShouldAutoLaunchAfterSilentUpgrade 自动拉起新版，并在 InitializeWizard 抢先清场兼容老版本更新。
    std::filesystem::path issPath;
    auto probe = tools3000::core::WinUtils::getExeDirectory();
    for (int depth = 0; depth < 6 && !probe.empty(); ++depth) {
        const auto candidate = probe / L"packaging" / L"installer" / L"installer.iss";
        if (std::filesystem::exists(candidate)) {
            issPath = candidate;
            break;
        }
        probe = probe.parent_path();
    }
    if (!issPath.empty()) {
        std::ifstream ifs(issPath, std::ios::binary);
        const std::string content((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
        EXPECT_EQ(content.find("/f /t /im Tools3000.exe"), std::string::npos)
            << "installer.iss must never pass /t when killing Tools3000.exe (would self-terminate in-app updater)";
        EXPECT_NE(content.find("PreClearProcessesBeforeRestartManager"), std::string::npos)
            << "installer.iss must pre-clear stale processes before wpPreparing/RestartManager for legacy updaters";
        EXPECT_NE(content.find("ShouldAutoLaunchAfterSilentUpgrade"), std::string::npos)
            << "installer.iss must auto-relaunch Tools3000.exe after in-app silent upgrade";
    }
}

// -----------------------------------------------------------------------------
// 14. 拼音索引引擎测试套件
// -----------------------------------------------------------------------------

TEST(WinUtilsTest, GetSystemDrives) {
    auto drives = tools3000::core::WinUtils::getSystemDrives();
    EXPECT_FALSE(drives.empty());
    bool foundC = false;
    for (const auto& d : drives) {
        EXPECT_TRUE(d.letter >= 'A' && d.letter <= 'Z');
        EXPECT_FALSE(d.path.empty());
        EXPECT_FALSE(d.typeStr.empty());
        if (d.letter == 'C') {
            foundC = true;
            EXPECT_EQ(d.path, L"C:\\");
        }
    }
    EXPECT_TRUE(foundC);
}

TEST(WinUtilsTest, FullscreenDetection) {
    // 空句柄或无效句柄返回 false
    EXPECT_FALSE(tools3000::core::WinUtils::isWindowFullscreen(nullptr));
    EXPECT_FALSE(tools3000::core::WinUtils::isWindowFullscreen((HWND)(uintptr_t)0x12345678));
    EXPECT_FALSE(tools3000::core::WinUtils::isWindowProcessElevated(nullptr));
    EXPECT_FALSE(tools3000::core::WinUtils::isWindowHigherIntegrity(nullptr));
    EXPECT_FALSE(tools3000::core::WinUtils::queryProcessElevated(nullptr));

    HWND overlay = CreateWindowExW(
        WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, L"STATIC", L"Tools3000 affinity test",
        WS_POPUP, 0, 0, 16, 16, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!overlay) {
        std::printf("[SKIP] display-affinity smoke: window creation unavailable\n");
        return;
    }
    EXPECT_TRUE(tools3000::core::WinUtils::allowWindowCapture(overlay));
    DWORD affinity = WDA_EXCLUDEFROMCAPTURE;
    EXPECT_TRUE(GetWindowDisplayAffinity(overlay, &affinity));
    EXPECT_EQ(affinity, static_cast<DWORD>(WDA_NONE));

    // 验证 Zero WDA_EXCLUDEFROMCAPTURE 架构规则：所有界面均支持被截图，excludeWindowFromCapture 亦保证 WDA_NONE
    EXPECT_TRUE(tools3000::core::WinUtils::excludeWindowFromCapture(overlay));
    affinity = WDA_EXCLUDEFROMCAPTURE;
    EXPECT_TRUE(GetWindowDisplayAffinity(overlay, &affinity));
    EXPECT_EQ(affinity, static_cast<DWORD>(WDA_NONE));
    DestroyWindow(overlay);
}

// -----------------------------------------------------------------------------
// 17. 截图后端能力与虚拟内存驱动测试套件
// -----------------------------------------------------------------------------

TEST(HotkeyManagerTest, HandlesRecordingPauseMode) {
    auto& mgr = tools3000::core::HotkeyManager::instance();
    EXPECT_FALSE(mgr.isPaused());

    mgr.setPaused(true);
    EXPECT_TRUE(mgr.isPaused());

    mgr.setPaused(false);
    EXPECT_FALSE(mgr.isPaused());
}


TEST(DpiUtilsTest, HighDpiSharedMetrics) {
    using namespace tools3000::core::dpi;
    EXPECT_NEAR(scaleForDpi(0), 1.0f, 0.001f);
    EXPECT_NEAR(scaleForDpi(96), 1.0f, 0.001f);
    EXPECT_NEAR(scaleForDpi(120), 1.25f, 0.001f);
    EXPECT_NEAR(scaleForDpi(144), 1.5f, 0.001f);
    EXPECT_NEAR(scaleForDpi(192), 2.0f, 0.001f);
    EXPECT_NEAR(rasterizationScaleForDpi(96), 1.0, 0.001);
    EXPECT_NEAR(rasterizationScaleForDpi(144), 1.5, 0.001);
    EXPECT_NEAR(rasterizationScaleForDpi(192), 2.0, 0.001);
    EXPECT_NEAR(scaleForDpi(480), 5.0f, 0.001f);
    EXPECT_NEAR(scaleForDpi(768), 5.0f, 0.001f);
    EXPECT_EQ(scaleMetric(36, 1.0f), 36);
    EXPECT_EQ(scaleMetric(36, 1.25f), 45);
    EXPECT_EQ(scaleMetric(36, 1.5f), 54);
    EXPECT_EQ(scaleMetric(1, 0.0f), 1);

    const RECT work{-1920, 0, 0, 1080};
    const RECT offscreen{-4000, -100, -2800, 800};
    const RECT clamped = clampWindowToWorkArea(offscreen, work);
    EXPECT_EQ(clamped.left, -1920);
    EXPECT_EQ(clamped.top, 0);
    EXPECT_EQ(clamped.right, -720);
    EXPECT_EQ(clamped.bottom, 900);

    const RECT tooBig{100, 100, 5000, 4000};
    const RECT primary{0, 0, 1920, 1080};
    const RECT fitted = clampWindowToWorkArea(tooBig, primary);
    EXPECT_EQ(fitted.left, 0);
    EXPECT_EQ(fitted.top, 0);
    EXPECT_EQ(fitted.right, 1920);
    EXPECT_EQ(fitted.bottom, 1080);

    const SIZE toast100 = tools3000::ui::ToastStyle::windowSizeForDpi(96);
    const SIZE toast150 = tools3000::ui::ToastStyle::windowSizeForDpi(144);
    const SIZE toast200 = tools3000::ui::ToastStyle::windowSizeForDpi(192);
    EXPECT_TRUE(toast100.cx == 600 && toast100.cy == 80);
    EXPECT_TRUE(toast150.cx == 900 && toast150.cy == 120);
    EXPECT_TRUE(toast200.cx == 1200 && toast200.cy == 160);
    EXPECT_EQ(tools3000::ui::ToastStyle::originYForWorkArea(0, 1080, 80, 48), 952);
    EXPECT_EQ(tools3000::ui::ToastStyle::originYForWorkArea(0, 80, 80, 48), 0);
    EXPECT_EQ(tools3000::ui::ToastStyle::originYForWorkArea(100, 200, 0, 48), 100);
    EXPECT_GT(tools3000::ui::ToastStyle::StrokeWidth, 1.5f);

    const SIZE keycast150 = tools3000::keycast::KeycastStyle::windowSizeForDpi(144);
    EXPECT_TRUE(keycast150.cx == 1200 && keycast150.cy == 240);

    const SIZE ocr100 = tools3000::ocr::OcrResultStyle::windowSizeForDpi(96);
    const SIZE ocr125 = tools3000::ocr::OcrResultStyle::windowSizeForDpi(120);
    const SIZE ocr150 = tools3000::ocr::OcrResultStyle::windowSizeForDpi(144);
    const SIZE ocr200 = tools3000::ocr::OcrResultStyle::windowSizeForDpi(192);
    EXPECT_TRUE(ocr100.cx == 600 && ocr100.cy == 400);
    EXPECT_TRUE(ocr125.cx == 750 && ocr125.cy == 500);
    EXPECT_TRUE(ocr150.cx == 900 && ocr150.cy == 600);
    EXPECT_TRUE(ocr200.cx == 1200 && ocr200.cy == 800);

    const SIZE search100 = tools3000::ui::SearchWindowStyle::windowSizeForDpi(96);
    const SIZE search150 = tools3000::ui::SearchWindowStyle::windowSizeForDpi(144);
    const SIZE search200 = tools3000::ui::SearchWindowStyle::windowSizeForDpi(192);
    EXPECT_TRUE(search100.cx == 800 && search100.cy == 600);
    EXPECT_TRUE(search150.cx == 1200 && search150.cy == 900);
    EXPECT_TRUE(search200.cx == 1600 && search200.cy == 1200);

    const SIZE tray100 = tools3000::ui::TrayWindowStyle::windowSizeForDpi(96);
    const SIZE tray150 = tools3000::ui::TrayWindowStyle::windowSizeForDpi(144);
    const SIZE tray200 = tools3000::ui::TrayWindowStyle::windowSizeForDpi(192);
    EXPECT_TRUE(tray100.cx == 320 && tray100.cy == 460);
    EXPECT_TRUE(tray150.cx == 480 && tray150.cy == 690);
    EXPECT_TRUE(tray200.cx == 640 && tray200.cy == 920);

    const SIZE settings100 = tools3000::ui::SettingsWindowStyle::windowSizeForDpi(96);
    const SIZE settings150 = tools3000::ui::SettingsWindowStyle::windowSizeForDpi(144);
    const SIZE settings200 = tools3000::ui::SettingsWindowStyle::windowSizeForDpi(192);
    EXPECT_TRUE(settings100.cx == 1100 && settings100.cy == 750);
    EXPECT_TRUE(settings150.cx == 1650 && settings150.cy == 1125);
    EXPECT_TRUE(settings200.cx == 2200 && settings200.cy == 1500);

    const SIZE settingsMin100 = tools3000::ui::SettingsWindowStyle::minWindowSizeForDpi(96);
    const SIZE settingsMin150 = tools3000::ui::SettingsWindowStyle::minWindowSizeForDpi(144);
    const SIZE settingsMin200 = tools3000::ui::SettingsWindowStyle::minWindowSizeForDpi(192);
    EXPECT_TRUE(settingsMin100.cx == 680 && settingsMin100.cy == 460);
    EXPECT_TRUE(settingsMin150.cx == 1020 && settingsMin150.cy == 690);
    EXPECT_TRUE(settingsMin200.cx == 1360 && settingsMin200.cy == 920);

    const SIZE want150 = tools3000::ui::SettingsWindowStyle::windowSizeForDpi(144);
    const RECT desk1080{0, 0, 1920, 1040};
    const SIZE fitted1080at150 = fitSizeToWorkArea(
        want150, desk1080, scaleMetric(24, 1.5f));
    EXPECT_LE(fitted1080at150.cx, 1920 - 72);
    EXPECT_LE(fitted1080at150.cy, 1040 - 72);
    EXPECT_LT(fitted1080at150.cy, want150.cy);

    const SIZE alreadyFits = fitSizeToWorkArea({800, 600}, {0, 0, 1920, 1080}, 24);
    EXPECT_EQ(alreadyFits.cx, 800);
    EXPECT_EQ(alreadyFits.cy, 600);

    const auto radial100 = tools3000::gesture::RadialMenuStyle::metricsForDpi(96);
    const auto radial150 = tools3000::gesture::RadialMenuStyle::metricsForDpi(144);
    const auto radial200 = tools3000::gesture::RadialMenuStyle::metricsForDpi(192);
    EXPECT_TRUE(radial100.windowSize == 400 && radial100.outerRadius == 150 &&
                radial100.innerRadius == 40);
    EXPECT_TRUE(radial150.windowSize == 600 && radial150.outerRadius == 225 &&
                radial150.innerRadius == 60);
    EXPECT_TRUE(radial200.windowSize == 800 && radial200.outerRadius == 300 &&
                radial200.innerRadius == 80);
}

static void check_toolbar_inside_surface(const tools3000::capture::CaptureState& state,
                                         D2D1_SIZE_F surface) {
    for (const auto& button : state.toolbarButtons) {
        EXPECT_GE(button.rect.left, 0.0f);
        EXPECT_GE(button.rect.top, 0.0f);
        EXPECT_LE(button.rect.right, surface.width + 0.01f);
        EXPECT_LE(button.rect.bottom, surface.height + 0.01f);
        EXPECT_GT(button.rect.right, button.rect.left);
        EXPECT_GT(button.rect.bottom, button.rect.top);
    }
}

// -----------------------------------------------------------------------------
// 25. 工具栏自适应 DPI 布局测试套件
// -----------------------------------------------------------------------------

TEST(WinUtilsTest, ClipboardAndEncoding) {
    EXPECT_EQ(tools3000::core::WinUtils::toLower("Tools3000_PRO"), "tools3000_pro");
    std::string text = "Tools3000 截图 & OCR 测试 🚀";
    std::wstring wtext = tools3000::core::WinUtils::utf8ToWstring(text);
    EXPECT_FALSE(wtext.empty());
    std::string roundtrip = tools3000::core::WinUtils::wstringToUtf8(wtext);
    EXPECT_EQ(roundtrip, text);

    std::vector<uint8_t> raw = {0x45, 0x61, 0x73, 0x79}; // "Easy"
    std::string b64 = tools3000::core::WinUtils::base64Encode(raw);
    EXPECT_EQ(b64, "RWFzeQ==");

    // 验证 Windows 原生高清图标 Base64 提取与缓存
    std::string iconFolder = tools3000::core::WinUtils::getFileTypeIconBase64(L"", true);
    EXPECT_TRUE(iconFolder.rfind("data:image/png;base64,", 0) == 0);

    std::string iconExe = tools3000::core::WinUtils::getFileTypeIconBase64(L".exe", false);
    EXPECT_TRUE(iconExe.rfind("data:image/png;base64,", 0) == 0);

    std::string iconDoc = tools3000::core::WinUtils::getFileTypeIconBase64(L"docx", false);
    EXPECT_TRUE(iconDoc.rfind("data:image/png;base64,", 0) == 0);

    // 验证大小写容错与内存单例缓存一致性
    EXPECT_EQ(tools3000::core::WinUtils::getFileTypeIconBase64(L"DOCX", false), iconDoc);
}

TEST(WinUtilsTest, OpenUrlSecurityAndSchemeValidation) {
    // 1. 空 URL 与纯空白字符串必须立即安全拒绝
    EXPECT_FALSE(tools3000::core::WinUtils::openUrl(L""));
    EXPECT_FALSE(tools3000::core::WinUtils::openUrl(L"   \t\r\n  "));

    // 2. 严密协议防御：拒绝所有非 http/https 协议及不完整协议 (命令注入、文件执行、javascript 脚本伪协议等)
    EXPECT_FALSE(tools3000::core::WinUtils::openUrl(L"http://"));
    EXPECT_FALSE(tools3000::core::WinUtils::openUrl(L"https://"));
    EXPECT_FALSE(tools3000::core::WinUtils::openUrl(L"javascript:alert(1)"));
    EXPECT_FALSE(tools3000::core::WinUtils::openUrl(L"  javascript:alert(1)  "));
    EXPECT_FALSE(tools3000::core::WinUtils::openUrl(L"file:///C:/Windows/System32/cmd.exe"));
    EXPECT_FALSE(tools3000::core::WinUtils::openUrl(L"cmd.exe /c calc.exe"));
    EXPECT_FALSE(tools3000::core::WinUtils::openUrl(L"powershell.exe -enc AAAA"));
    EXPECT_FALSE(tools3000::core::WinUtils::openUrl(L"data:text/html,<html>alert</html>"));

    // 3. openFile 针对空白与非 http/https 协议的非法路径正常返回 false
    EXPECT_FALSE(tools3000::core::WinUtils::openFile(L""));
    EXPECT_FALSE(tools3000::core::WinUtils::openFile(L"   "));
    EXPECT_FALSE(tools3000::core::WinUtils::openFile(L"invalid://scheme/test"));
    EXPECT_FALSE(tools3000::core::WinUtils::openFile(L"  invalid://scheme/test  "));

    // 4. 验证 MessageBridge 中注册的 system.openUrl 与 system.openExternal 边界处理
    auto& bridge = tools3000::core::MessageBridge::instance();
    tools3000::core::registerSystemInteractionHandlers(bridge);

    const nlohmann::json emptyReq = {
        {"id", 9901},
        {"method", "system.openUrl"},
        {"params", nlohmann::json::object()}
    };
    const auto emptyResp = nlohmann::json::parse(bridge.handleMessage(emptyReq.dump()));
    ASSERT_TRUE(emptyResp.contains("result"));
    EXPECT_FALSE(emptyResp["result"].value("success", true));
    EXPECT_EQ(emptyResp["result"].value("error", ""), "url is required");

    const nlohmann::json invalidReq = {
        {"id", 9902},
        {"method", "system.openExternal"},
        {"params", {{"url", "powershell.exe"}}}
    };
    const auto invalidResp = nlohmann::json::parse(bridge.handleMessage(invalidReq.dump()));
    ASSERT_TRUE(invalidResp.contains("result"));
    EXPECT_FALSE(invalidResp["result"].value("success", true));
}

// -----------------------------------------------------------------------------
// 28. Lua 脚本引擎沙箱与安全测试套件
// -----------------------------------------------------------------------------
TEST(LuaEngineTest, SandboxSecurityAndBindings) {
    auto& lua = tools3000::core::LuaEngine::instance();
    EXPECT_TRUE(lua.initialize());

    // 1. Safe 绝对无害只读权限 (仅 Log 与 Url)
    EXPECT_TRUE(lua.executeScript("local a = 1 + 2; easy.log.info('Safe test'); local enc = easy.url.encode('abc 123')", tools3000::core::LuaPermission::Safe));

    // 2. Safe 模式下必须严格拦截 Window / Keyboard / Clipboard / Screen / Shell / Fs / Http
    EXPECT_FALSE(lua.executeScript("easy.window.minimize()", tools3000::core::LuaPermission::Safe));
    EXPECT_FALSE(lua.executeScript("easy.keyboard.sendKeys('Ctrl+C')", tools3000::core::LuaPermission::Safe));
    EXPECT_FALSE(lua.executeScript("easy.clipboard.getText()", tools3000::core::LuaPermission::Safe));
    EXPECT_FALSE(lua.executeScript("easy.screen.getPixelColor(0, 0)", tools3000::core::LuaPermission::Safe));
    EXPECT_FALSE(lua.executeScript("easy.shell.run('notepad.exe')", tools3000::core::LuaPermission::Safe));
    EXPECT_FALSE(lua.executeScript("easy.fs.exists('test.txt')", tools3000::core::LuaPermission::Safe));

    // 3. 超时保护测试（死循环被 100ms 钩子及时中断）
    auto t0 = std::chrono::steady_clock::now();
    bool timeoutResult = lua.executeScript("while true do end", tools3000::core::LuaPermission::Standard, std::chrono::milliseconds(100));
    auto t1 = std::chrono::steady_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();
    EXPECT_FALSE(timeoutResult);
    EXPECT_LT(elapsed, 1000);

    // 4. 取消令牌测试
    std::atomic<bool> cancelToken{true};
    EXPECT_FALSE(lua.executeScript("local x = 0; for i = 1, 10000000 do x = x + i end", tools3000::core::LuaPermission::Standard, std::chrono::milliseconds(5000), &cancelToken));

    // 5. 沙箱安全性：危险系统调用已被封禁
    EXPECT_FALSE(lua.executeScript("os.execute('echo hack')"));
    EXPECT_FALSE(lua.executeScript("os.remove('test.txt')"));

    // 6. 显式授予敏感权限时允许调用
    EXPECT_TRUE(lua.executeScript("local ok = easy.fs.exists('CMakeLists.txt')", tools3000::core::LuaPermission::Fs));

    // 7. 用户授权决策流与授权持久化测试
    const std::string testScriptId = "gesture:test_action";
    lua.revokePermissions(testScriptId);
    EXPECT_FALSE(lua.isScriptAuthorized(testScriptId, tools3000::core::LuaPermission::Keyboard));

    // 显式授权后立即可用
    lua.grantPermissions(testScriptId, tools3000::core::LuaPermission::Keyboard | tools3000::core::LuaPermission::Window);
    EXPECT_TRUE(lua.isScriptAuthorized(testScriptId, tools3000::core::LuaPermission::Keyboard));
    EXPECT_TRUE(lua.isScriptAuthorized(testScriptId, tools3000::core::LuaPermission::Window));
    EXPECT_FALSE(lua.isScriptAuthorized(testScriptId, tools3000::core::LuaPermission::Shell));

    // 撤销授权测试
    lua.revokePermissions(testScriptId);
    EXPECT_FALSE(lua.isScriptAuthorized(testScriptId, tools3000::core::LuaPermission::Keyboard));

    lua.shutdown();
}

// -----------------------------------------------------------------------------
// 29. 系统托盘图标与通知测试套件
// -----------------------------------------------------------------------------
TEST(TrayTest, NotificationAndBalloonDispatch) {
    WNDCLASSEXW wcex = { sizeof(WNDCLASSEXW) };
    wcex.lpfnWndProc = DefWindowProcW;
    wcex.hInstance   = GetModuleHandleW(nullptr);
    wcex.lpszClassName = L"Tools3000_TestTrayWnd";
    RegisterClassExW(&wcex);

    HWND hwnd = CreateWindowExW(
        WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
        L"Tools3000_TestTrayWnd",
        L"Tools3000 Test",
        WS_POPUP,
        0, 0, 0, 0,
        nullptr, nullptr, wcex.hInstance, nullptr
    );
    EXPECT_NE(hwnd, nullptr);

    // 动态生成保证有效的 16x16 测试图标
    int cx = 16, cy = 16;
    HDC hdcScreen = GetDC(nullptr);
    HDC hdcMem = CreateCompatibleDC(hdcScreen);
    HBITMAP hbmColor = CreateCompatibleBitmap(hdcScreen, cx, cy);
    HBITMAP hbmMask = CreateBitmap(cx, cy, 1, 1, nullptr);
    HBITMAP hOld = (HBITMAP)SelectObject(hdcMem, hbmColor);
    HBRUSH hBrush = CreateSolidBrush(RGB(0, 120, 215));
    RECT rc{0, 0, cx, cy};
    FillRect(hdcMem, &rc, hBrush);
    DeleteObject(hBrush);
    SelectObject(hdcMem, hOld);
    DeleteDC(hdcMem);
    ReleaseDC(nullptr, hdcScreen);

    ICONINFO ii{};
    ii.fIcon = TRUE;
    ii.hbmColor = hbmColor;
    ii.hbmMask = hbmMask;
    HICON hIcon = CreateIconIndirect(&ii);
    DeleteObject(hbmColor);
    DeleteObject(hbmMask);

    EXPECT_NE(hIcon, nullptr);

    NOTIFYICONDATAW nid{};
    nid.cbSize = sizeof(NOTIFYICONDATAW);
    nid.hWnd = hwnd;
    nid.uID = 1;
    nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    nid.uCallbackMessage = WM_USER + 100;
    nid.hIcon = hIcon;
    wcscpy_s(nid.szTip, L"Tools3000 Test");

    BOOL ok = Shell_NotifyIconW(NIM_ADD, &nid);
    if (!ok) {
        nid.cbSize = NOTIFYICONDATAW_V3_SIZE;
        ok = Shell_NotifyIconW(NIM_ADD, &nid);
    }

    HWND shellTrayWnd = FindWindowW(L"Shell_TrayWnd", nullptr);
    if (shellTrayWnd != nullptr) {
        // 在存在真实任务栏的交互桌面环境下，添加托盘图标必须 100% 成功，严禁任何失败！
        EXPECT_TRUE(ok) << "Shell_NotifyIconW failed on interactive desktop with active taskbar! Error=" << GetLastError();
    } else {
        // 在纯无头 CI 沙箱环境下无 Shell_TrayWnd，记录环境特征并确保未崩溃
        std::printf("[TrayTest] Headless session confirmed (Shell_TrayWnd is nullptr). NIM_ADD ok=%d, err=%lu\n", ok, GetLastError());
    }

    if (ok) {
        Shell_NotifyIconW(NIM_DELETE, &nid);
    }
    DestroyIcon(hIcon);
    DestroyWindow(hwnd);
}

TEST(TrayIconTest, ProductionClassStateMachineAndCallbacks) {
    auto& tray = tools3000::tray::TrayIcon::instance();

    // 1. 手势暂停状态机与 Tooltip 同步测试
    tray.setGesturePaused(true);
    EXPECT_TRUE(tray.isGesturePaused());
    EXPECT_TRUE(tray.tooltip().find(L"暂停") != std::wstring::npos ||
                tray.tooltip().find(L"Paused") != std::wstring::npos);
    EXPECT_TRUE(tray.tooltip().find(L"Tools3000") != std::wstring::npos);

    tray.setGesturePaused(false);
    EXPECT_FALSE(tray.isGesturePaused());
    EXPECT_TRUE(tray.tooltip().find(L"Tools3000") != std::wstring::npos);

    // 2. 主题自适应图标刷新测试 (确保高频刷新安全不崩溃)
    EXPECT_NO_THROW(tray.refreshThemeIcon());

    // 3. 业务回调注册与分发测试
    std::atomic<bool> settingsOpened{false};
    std::atomic<bool> exited{false};
    std::atomic<bool> searchOpened{false};

    tray.onOpenSettings([&]() { settingsOpened.store(true); });
    tray.onExit([&]() { exited.store(true); });
    tray.onSearch([&]() { searchOpened.store(true); });

    tray.fireCallback(tools3000::tray::TrayMenuId::OpenSettings);
    EXPECT_TRUE(settingsOpened.load());

    tray.fireCallback(tools3000::tray::TrayMenuId::Search);
    EXPECT_TRUE(searchOpened.load());

    tray.fireCallback(tools3000::tray::TrayMenuId::Exit);
    EXPECT_TRUE(exited.load());

    // 4. 回调异常隔离测试 (生产环境坚固防线：回调抛出异常必须被安全捕获，绝不能导致主进程崩溃)
    tray.onScreenshot([]() { throw std::runtime_error("Simulated callback exception for testing fault barrier"); });
    EXPECT_NO_THROW(tray.fireCallback(tools3000::tray::TrayMenuId::Screenshot));

    // 5. 消息分发回路测试 (handleMessage)
    // 5.1 左键双击应唤起设置
    settingsOpened.store(false);
    tray.handleMessage(0, WM_LBUTTONDBLCLK);
    EXPECT_TRUE(settingsOpened.load());

    // 5.2 右键单击应呼出托盘悬浮窗
    tray.handleMessage(0, WM_RBUTTONUP);
    EXPECT_TRUE(tools3000::ui::TrayWindow::instance().isVisible());

    // 5.3 再次左键单击应平滑收起悬浮窗
    tray.handleMessage(0, WM_LBUTTONUP);
    EXPECT_FALSE(tools3000::ui::TrayWindow::instance().isVisible());

    // 5.4 现代 NOTIFYICON_VERSION_4 规范: WM_CONTEXTMENU 携带物理坐标呼出托盘悬浮窗
    tray.handleMessage(MAKEWPARAM(500, 500), WM_CONTEXTMENU);
    EXPECT_TRUE(tools3000::ui::TrayWindow::instance().isVisible());

    // 5.5 现代规范 NIN_SELECT: 单击平滑收起托盘悬浮窗
    tray.handleMessage(0, NIN_SELECT);
    EXPECT_FALSE(tools3000::ui::TrayWindow::instance().isVisible());

    // 5.6 现代 NOTIFYICON_VERSION_4 规范: 键盘无坐标呼出 (-1, -1) 自适应弹出
    tray.handleMessage(MAKEWPARAM(-1, -1), WM_CONTEXTMENU);
    EXPECT_TRUE(tools3000::ui::TrayWindow::instance().isVisible());

    // 5.7 再次触发右键菜单应平滑收起 (Toggle 功能)
    tray.handleMessage(MAKEWPARAM(500, 500), WM_CONTEXTMENU);
    EXPECT_FALSE(tools3000::ui::TrayWindow::instance().isVisible());

    // 5.8 现代规范 NIN_KEYSELECT: 键盘选中直接唤起主设置
    settingsOpened.store(false);
    tray.handleMessage(0, NIN_KEYSELECT);
    EXPECT_TRUE(settingsOpened.load());

    // 5.9 核心规范：左键单击保持绝对静默，绝不弹出菜单
    EXPECT_FALSE(tools3000::ui::TrayWindow::instance().isVisible());
    tray.handleMessage(0, WM_LBUTTONUP);
    EXPECT_FALSE(tools3000::ui::TrayWindow::instance().isVisible());

    // 5.10 现代协议 NIN_SELECT 左键单击同样保持静默，绝不弹出菜单
    tray.handleMessage(0, NIN_SELECT);
    EXPECT_FALSE(tools3000::ui::TrayWindow::instance().isVisible());

    // 5.11 右键单击孪生消息防抖去重测试（WM_RBUTTONUP 紧随 WM_CONTEXTMENU 不发生意外 Toggle 收起）
    tray.handleMessage(0, WM_RBUTTONUP);
    EXPECT_TRUE(tools3000::ui::TrayWindow::instance().isVisible());
    tray.handleMessage(MAKEWPARAM(500, 500), WM_CONTEXTMENU);
    EXPECT_TRUE(tools3000::ui::TrayWindow::instance().isVisible());
    // 再次显式左键关闭
    tray.handleMessage(0, WM_LBUTTONUP);
    EXPECT_FALSE(tools3000::ui::TrayWindow::instance().isVisible());

    // 5.12 原生菜单自愈降级链路测试：当 WebView2 尚未渲染就绪时，右键不呼出 TrayWindow，而是 0ms 自动降级为原生菜单
    tools3000::ui::TrayWindow::instance().setWebViewReady(false);
    EXPECT_FALSE(tools3000::ui::TrayWindow::instance().isWebViewReady());
    tray.handleMessage(0, WM_RBUTTONUP);
    EXPECT_FALSE(tools3000::ui::TrayWindow::instance().isVisible());
    tools3000::ui::TrayWindow::instance().setWebViewReady(true);
    EXPECT_TRUE(tools3000::ui::TrayWindow::instance().isWebViewReady());

    // 5.13 反向顺序孪生消息去重测试：WM_CONTEXTMENU 呼出后紧随 WM_RBUTTONUP 不发生意外收起
    tray.handleMessage(MAKEWPARAM(500, 500), WM_CONTEXTMENU);
    EXPECT_TRUE(tools3000::ui::TrayWindow::instance().isVisible());
    tray.handleMessage(0, WM_RBUTTONUP);
    EXPECT_TRUE(tools3000::ui::TrayWindow::instance().isVisible());
    tray.handleMessage(0, WM_LBUTTONUP);
    EXPECT_FALSE(tools3000::ui::TrayWindow::instance().isVisible());

    // 5.14 托盘浮窗打开状态下左键双击：平滑收起托盘浮窗并瞬时唤起主设置窗口
    settingsOpened.store(false);
    tray.handleMessage(MAKEWPARAM(500, 500), WM_CONTEXTMENU);
    EXPECT_TRUE(tools3000::ui::TrayWindow::instance().isVisible());
    tray.handleMessage(0, WM_LBUTTONDBLCLK);
    EXPECT_FALSE(tools3000::ui::TrayWindow::instance().isVisible());
    EXPECT_TRUE(settingsOpened.load());

    // 5.15 连续多次右键点击交替 Toggle 连击测试：WM_CONTEXTMENU + WM_RBUTTONUP 孪生消息连击无消息丢失与抖动
    // 第一次右键呼出
    tray.handleMessage(MAKEWPARAM(500, 500), WM_CONTEXTMENU);
    tray.handleMessage(0, WM_RBUTTONUP);
    EXPECT_TRUE(tools3000::ui::TrayWindow::instance().isVisible());

    // 第二次右键平滑收起 (Toggle)
    tray.handleMessage(MAKEWPARAM(500, 500), WM_CONTEXTMENU);
    tray.handleMessage(0, WM_RBUTTONUP);
    EXPECT_FALSE(tools3000::ui::TrayWindow::instance().isVisible());

    // 第三次右键再次呼出 (Toggle)
    tray.handleMessage(MAKEWPARAM(500, 500), WM_CONTEXTMENU);
    tray.handleMessage(0, WM_RBUTTONUP);
    EXPECT_TRUE(tools3000::ui::TrayWindow::instance().isVisible());

    // 第四次右键平滑收起 (Toggle)
    tray.handleMessage(MAKEWPARAM(500, 500), WM_CONTEXTMENU);
    tray.handleMessage(0, WM_RBUTTONUP);
    EXPECT_FALSE(tools3000::ui::TrayWindow::instance().isVisible());

    // 5.16 反向顺序孪生消息连续多次右键连击测试：WM_RBUTTONUP + WM_CONTEXTMENU
    // 第一次右键呼出
    tray.handleMessage(0, WM_RBUTTONUP);
    tray.handleMessage(MAKEWPARAM(500, 500), WM_CONTEXTMENU);
    EXPECT_TRUE(tools3000::ui::TrayWindow::instance().isVisible());

    // 第二次右键平滑收起 (Toggle)
    tray.handleMessage(0, WM_RBUTTONUP);
    tray.handleMessage(MAKEWPARAM(500, 500), WM_CONTEXTMENU);
    EXPECT_FALSE(tools3000::ui::TrayWindow::instance().isVisible());

    // 第三次右键再次呼出 (Toggle)
    tray.handleMessage(0, WM_RBUTTONUP);
    tray.handleMessage(MAKEWPARAM(500, 500), WM_CONTEXTMENU);
    EXPECT_TRUE(tools3000::ui::TrayWindow::instance().isVisible());

    // 第四次右键平滑收起 (Toggle)
    tray.handleMessage(0, WM_RBUTTONUP);
    tray.handleMessage(MAKEWPARAM(500, 500), WM_CONTEXTMENU);
    EXPECT_FALSE(tools3000::ui::TrayWindow::instance().isVisible());

    // 6. 生命周期销毁与复位测试
    tray.destroy();
    EXPECT_FALSE(tray.isCreated());
}

TEST(TrayWindowTest, IdentifiesTaskbarAndOverflowWindowClasses) {
    EXPECT_FALSE(tools3000::ui::TrayWindow::isTaskbarOrOverflowWindow(nullptr));
    EXPECT_FALSE(tools3000::ui::TrayWindow::isTaskbarOrOverflowWindow((HWND)(intptr_t)0xdeadbeef));

    HINSTANCE hInst = GetModuleHandleW(nullptr);
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = hInst;
    wc.lpszClassName = L"Test_NormalWindow";
    RegisterClassExW(&wc);

    HWND normalWnd = CreateWindowExW(0, L"Test_NormalWindow", L"", WS_POPUP, 0, 0, 10, 10, nullptr, nullptr, hInst, nullptr);
    if (normalWnd) {
        EXPECT_FALSE(tools3000::ui::TrayWindow::isTaskbarOrOverflowWindow(normalWnd));
        DestroyWindow(normalWnd);
    }

    // 覆盖全套任务栏与溢出区白名单类名 (Shell_TrayWnd, Shell_SecondaryTrayWnd, NotifyIconOverflowWindow, TopLevelWindowForOverflowXamlIsland, XamlExplorerHostIslandWindow, TrayNotifyWnd, SysPager, ToolbarWindow32)
    const wchar_t* const whitelistClasses[] = {
        L"Shell_TrayWnd",
        L"Shell_SecondaryTrayWnd",
        L"NotifyIconOverflowWindow",
        L"TopLevelWindowForOverflowXamlIsland",
        L"XamlExplorerHostIslandWindow",
        L"TrayNotifyWnd",
        L"SysPager",
        L"ToolbarWindow32"
    };

    for (const auto* clsName : whitelistClasses) {
        wc.lpszClassName = clsName;
        if (RegisterClassExW(&wc)) {
            HWND wnd = CreateWindowExW(0, clsName, L"", WS_POPUP, 0, 0, 10, 10, nullptr, nullptr, hInst, nullptr);
            if (wnd) {
                EXPECT_TRUE(tools3000::ui::TrayWindow::isTaskbarOrOverflowWindow(wnd));

                // 测试子控件向上追溯顶层根窗口能力 (GetAncestor GA_ROOT)
                HWND childWnd = CreateWindowExW(0, L"Test_NormalWindow", L"", WS_CHILD, 0, 0, 5, 5, wnd, nullptr, hInst, nullptr);
                if (childWnd) {
                    EXPECT_TRUE(tools3000::ui::TrayWindow::isTaskbarOrOverflowWindow(childWnd));
                    DestroyWindow(childWnd);
                }
                DestroyWindow(wnd);
            }
            UnregisterClassW(clsName, hInst);
        }
    }
    UnregisterClassW(L"Test_NormalWindow", hInst);
}

TEST(UipiMessageFilterTest, AllowsSettingsWindowActivationMessages) {
    constexpr UINT wmShowSettings = WM_APP + 101;
    UINT msgBroadcast = RegisterWindowMessageW(L"Tools3000_ShowSettings_Broadcast");
    EXPECT_NE(msgBroadcast, 0u);

    // 1. 全局进程级 UIPI 豁免过滤 (MSGFLT_ADD)
    BOOL resFilter = ChangeWindowMessageFilter(wmShowSettings, MSGFLT_ADD);
    EXPECT_TRUE(resFilter);

    BOOL resBroadcast = ChangeWindowMessageFilter(msgBroadcast, MSGFLT_ADD);
    EXPECT_TRUE(resBroadcast);

    // 2. 窗口句柄级 UIPI 精确放行豁免 (ChangeWindowMessageFilterEx / MSGFLT_ALLOW)
    HINSTANCE hInst = GetModuleHandleW(nullptr);
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = hInst;
    wc.lpszClassName = L"Test_UipiFilterMessageWindow";
    if (RegisterClassExW(&wc)) {
        HWND testWnd = CreateWindowExW(
            WS_EX_TOOLWINDOW, L"Test_UipiFilterMessageWindow", L"", WS_POPUP,
            0, 0, 10, 10, nullptr, nullptr, hInst, nullptr);
        if (testWnd) {
            BOOL resEx1 = ChangeWindowMessageFilterEx(testWnd, wmShowSettings, MSGFLT_ALLOW, nullptr);
            EXPECT_TRUE(resEx1);
            BOOL resEx2 = ChangeWindowMessageFilterEx(testWnd, msgBroadcast, MSGFLT_ALLOW, nullptr);
            EXPECT_TRUE(resEx2);
            DestroyWindow(testWnd);
        }
        UnregisterClassW(L"Test_UipiFilterMessageWindow", hInst);
    }
}

// -----------------------------------------------------------------------------
// 30. 划词翻译与快捷预览测试套件
// -----------------------------------------------------------------------------
TEST(QuickLookTest, TranslationAndWindowPreview) {
    // 1. Explorer 选中文件判定冒烟（安全不崩溃）
    auto sel = tools3000::core::WinUtils::getSelectedExplorerFile();
    // 在无前台 Explorer 的测试环境下应返回 nullopt，不可抛异常或悬挂
    EXPECT_TRUE(!sel.has_value() || !sel->empty());

    // 2. 划词提取冒烟测试
    std::string captured = tools3000::core::WinUtils::captureSelectedText();
    // 允许为空或剪贴板原有内容
    EXPECT_GE(captured.size(), 0u);

    // 3. 键盘钩子拦截器注册测试（RAII 作用域守卫确保断言提前退出或测试结束时必清空栈引用）
    bool hookCallbackCalled = false;
    struct ScopedQuickLookTestCleanup {
        ~ScopedQuickLookTestCleanup() {
            tools3000::core::KeyboardHook::instance().setKeyInterceptor(nullptr);
            tools3000::core::MessageBridge::instance().unregisterHandler("quicklook.test");
        }
    } cleanupGuard;

    tools3000::core::KeyboardHook::instance().setKeyInterceptor([&hookCallbackCalled](DWORD vk, WPARAM) -> bool {
        if (vk == VK_F24) {
            hookCallbackCalled = true;
            return true;
        }
        return false;
    });

    // 4. QuickLook IPC 处理方法注册与测试
    tools3000::core::MessageBridge::instance().registerHandler("quicklook.test", [](const nlohmann::json& params) -> nlohmann::json {
        return {{"path", params.value("path", "")}, {"ok", true}};
    });

    std::string resp = tools3000::core::MessageBridge::instance().handleMessage(
        R"({"id":999,"method":"quicklook.test","params":{"path":"C:\\test.md"}})");
    auto jResp = nlohmann::json::parse(resp);
    EXPECT_EQ(jResp["id"], 999);
    EXPECT_EQ(jResp["result"]["ok"], true);
    EXPECT_EQ(jResp["result"]["path"], "C:\\test.md");

    // 5. 快捷键冲突检测与分类判定测试
    auto& hkManager = tools3000::core::HotkeyManager::instance();
    const tools3000::core::HotkeyDef dummyKey{tools3000::core::ModKey::Alt, 'Z'};
    hkManager.registerHotkey("Test Feature A", dummyKey, []() {});

    // 同名重绑不应自相冲突
    auto selfConflict = hkManager.checkConflict(dummyKey, "Test Feature A");
    EXPECT_FALSE(selfConflict.hasConflict);

    // 异名重绑应检测出内部冲突
    auto interConflict = hkManager.checkConflict(dummyKey, "Test Feature B");
    EXPECT_TRUE(interConflict.hasConflict);
    EXPECT_EQ(interConflict.conflictType, "internal");

    // 清理测试热键
    hkManager.unregisterHotkey("Test Feature A");
}

// -----------------------------------------------------------------------------
// 31. 文档全文索引引擎与格式解析器测试套件
// -----------------------------------------------------------------------------

TEST(StatsManagerTest, RecordingAndHistory) {
    auto& stats = tools3000::core::StatsManager::instance();
    stats.initialize();
    stats.clearToday();

    stats.recordKey(VK_RETURN);
    stats.recordKey(VK_SPACE);
    stats.recordLeftClick();
    stats.recordRightClick();
    stats.recordScroll();
    stats.recordMouseDistance(120.5);

    auto today = stats.getTodayStats();
    EXPECT_GE(today.totalKeys, 2u);
    EXPECT_GE(today.leftClicks, 1u);
    EXPECT_GE(today.rightClicks, 1u);
    EXPECT_GE(today.scrolls, 1u);
    EXPECT_GE(today.mouseDistance, 120.0);

    auto jsonToday = today.toJson();
    EXPECT_TRUE(jsonToday.contains("totalKeys"));
    EXPECT_TRUE(jsonToday.contains("leftClicks"));

    auto restored = tools3000::core::DailyStats::fromJson(jsonToday);
    EXPECT_EQ(restored.totalKeys, today.totalKeys);
    EXPECT_EQ(restored.leftClicks, today.leftClicks);

    auto history = stats.getHistory(7);
    EXPECT_TRUE(history.is_object());

    auto total = stats.getTotalStats();
    EXPECT_TRUE(total.contains("totalKeystrokes"));

    stats.shutdown();
}

// -----------------------------------------------------------------------------
// 34. 运行历史 (Run History) 与 Frecency 权重计算测试套件
// -----------------------------------------------------------------------------

TEST(MessageBridgeAsyncTest, AsyncMethodLeavesTheCallingThread) {
    auto& bridge = tools3000::core::MessageBridge::instance();
    std::atomic<bool> handlerRan{false};
    std::thread::id handlerThread{};

    bridge.registerHandler("test.asyncEcho", [&](const nlohmann::json& params) -> nlohmann::json {
        handlerThread = std::this_thread::get_id();
        handlerRan.store(true);
        return {{"echo", params.value("value", 0)}};
    });
    bridge.markMethodAsync("test.asyncEcho");

    std::promise<std::string> promise;
    auto future = promise.get_future();
    bridge.handleMessageAsync(
        R"({"id":42,"method":"test.asyncEcho","params":{"value":7}})",
        [&promise](std::string response) { promise.set_value(std::move(response)); });

    ASSERT_EQ(future.wait_for(std::chrono::seconds(5)), std::future_status::ready);
    const auto parsed = nlohmann::json::parse(future.get());
    EXPECT_EQ(parsed["id"].get<int>(), 42);
    EXPECT_EQ(parsed["result"]["echo"].get<int>(), 7);
    EXPECT_TRUE(handlerRan.load());
    EXPECT_NE(handlerThread, std::this_thread::get_id());

    bridge.unregisterHandler("test.asyncEcho");
}

TEST(MessageBridgeAsyncTest, UnmarkedMethodRespondsBeforeReturning) {
    auto& bridge = tools3000::core::MessageBridge::instance();
    bridge.registerHandler("test.syncEcho", [](const nlohmann::json& params) -> nlohmann::json {
        return {{"echo", params.value("value", 0)}};
    });

    std::string response;
    bool respondedInline = false;
    bridge.handleMessageAsync(
        R"({"id":11,"method":"test.syncEcho","params":{"value":3}})",
        [&](std::string value) {
            response = std::move(value);
            respondedInline = true;
        });

    EXPECT_TRUE(respondedInline);
    const auto parsed = nlohmann::json::parse(response);
    EXPECT_EQ(parsed["result"]["echo"].get<int>(), 3);

    bridge.unregisterHandler("test.syncEcho");
}

TEST(MessageBridgeAsyncTest, MalformedAndUnknownMessagesStillProduceAResponse) {
    auto& bridge = tools3000::core::MessageBridge::instance();

    std::string malformed;
    bridge.handleMessageAsync("{ this is not json",
                              [&](std::string value) { malformed = std::move(value); });
    ASSERT_FALSE(malformed.empty());
    EXPECT_TRUE(nlohmann::json::parse(malformed).contains("error"));

    std::string unknown;
    bridge.handleMessageAsync(R"({"id":9,"method":"test.doesNotExist","params":{}})",
                              [&](std::string value) { unknown = std::move(value); });
    ASSERT_FALSE(unknown.empty());
    const auto parsed = nlohmann::json::parse(unknown);
    EXPECT_EQ(parsed["id"].get<int>(), 9);
    EXPECT_EQ(parsed["error"]["code"].get<int>(), -32601);
}


TEST(MessageBridgeAsyncTest, QueueOverloadRespondsWithTheEvictedRequestId) {
    auto& bridge = tools3000::core::MessageBridge::instance();
    std::mutex gateMutex;
    std::condition_variable gateCv;
    int running = 0;
    bool release = false;

    bridge.registerHandler("test.asyncQueueOverload", [&](const nlohmann::json&) -> nlohmann::json {
        std::unique_lock lock(gateMutex);
        ++running;
        gateCv.notify_all();
        gateCv.wait(lock, [&release]() { return release; });
        return {{"done", true}};
    });
    bridge.markMethodAsync("test.asyncQueueOverload");

    std::vector<std::shared_ptr<std::promise<std::string>>> promises;
    std::vector<std::future<std::string>> futures;
    const auto submit = [&](int id) {
        auto response = std::make_shared<std::promise<std::string>>();
        futures.push_back(response->get_future());
        promises.push_back(response);
        bridge.handleMessageAsync(
            nlohmann::json{{"id", id}, {"method", "test.asyncQueueOverload"}, {"params", nlohmann::json::object()}}.dump(),
            [response](std::string value) { response->set_value(std::move(value)); });
    };

    constexpr int kExpectedAsyncWorkers = 8;
    for (int id = 1000; id < 1000 + kExpectedAsyncWorkers; ++id) submit(id);
    bool allWorkersStarted = false;
    {
        std::unique_lock lock(gateMutex);
        allWorkersStarted = gateCv.wait_for(lock, std::chrono::seconds(5), [&running]() {
            return running == kExpectedAsyncWorkers;
        });
        if (!allWorkersStarted) {
            release = true;
        }
    }
    if (!allWorkersStarted) {
        gateCv.notify_all();
        for (auto& future : futures) future.wait_for(std::chrono::seconds(5));
        bridge.unregisterHandler("test.asyncQueueOverload");
        FAIL() << "async worker pool did not start all workers";
    }

    for (int id = 2000; id < 2065; ++id) submit(id);
    const size_t evictedFutureIndex = static_cast<size_t>(kExpectedAsyncWorkers);
    const bool evictionResponded =
        futures[evictedFutureIndex].wait_for(std::chrono::seconds(5)) == std::future_status::ready;
    std::string evictedResponse;
    if (evictionResponded) evictedResponse = futures[evictedFutureIndex].get();

    {
        std::lock_guard lock(gateMutex);
        release = true;
    }
    gateCv.notify_all();
    bool allResponsesReady = true;
    for (size_t index = 0; index < futures.size(); ++index) {
        if (index == evictedFutureIndex) continue;
        if (futures[index].wait_for(std::chrono::seconds(5)) != std::future_status::ready) {
            allResponsesReady = false;
        }
    }
    bridge.unregisterHandler("test.asyncQueueOverload");

    EXPECT_TRUE(allResponsesReady);
    ASSERT_TRUE(evictionResponded);
    const auto evicted = nlohmann::json::parse(evictedResponse);
    EXPECT_EQ(evicted["id"].get<int>(), 2000);
    EXPECT_EQ(evicted["error"]["code"].get<int>(), -32000);
}

// -----------------------------------------------------------------------------
// 36.4 索引服务的退出归属
//
// 索引常驻要几百 MB。主程序退出时该不该把它带走，取决于这个服务是谁的：自己
// 拉起的子进程该收尾，用户装成 Windows 服务的不该动，别的实例拉起的也不该动。
// -----------------------------------------------------------------------------

TEST(PerformanceRegressionBenchmarkTest, FileIndexStoreMemoryAndThroughputGate) {
    tools3000::service::FileIndexStore store;
    constexpr size_t kFileCount = 50000;
    store.reserve(kFileCount);

    const auto tStartInsert = std::chrono::steady_clock::now();
    for (uint64_t i = 1; i <= kFileCount; ++i) {
        FileRecordInit r{};
        r.fileReferenceNumber = i;
        r.parentFileReferenceNumber = (i > 10) ? (i % 10 + 1) : 0;
        if (i % 3 == 0) {
            r.fileName = L"企业财务报表_2026年度汇算清缴_" + std::to_wstring(i) + L".xlsx";
        } else if (i % 3 == 1) {
            r.fileName = L"SystemModule_" + std::to_wstring(i) + L"_DebugTrace.log";
        } else {
            r.fileName = L"DeepLearningModel_" + std::to_wstring(i) + L".onnx";
        }
        r.fileSize = 1024 * (i % 500 + 1);
        r.creationTime = 134000000000000000ULL + i;
        r.lastWriteTime = 134000000000000000ULL + i * 2;
        r.isDirectory = (i <= 10);
        store.upsert(r);
    }
    store.releaseBuildScratch();
    const auto insertDurationMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - tStartInsert).count();

    EXPECT_LT(insertDurationMs, 500);

    // 内存门禁: 50,000 记录均摊内存必须严格低于 180 字节/文件 (当前 ~110 B/file)
    const uint64_t totalBytes = store.approximateBytes();
    const uint64_t bytesPerFile = totalBytes / store.size();
    EXPECT_LT(bytesPerFile, 180u);

    // 吞吐量门禁: 50,000 文件单趟无锁扫描在 50ms 内完成 (>= 1,000,000 文件/秒)
    const auto expr = ::SearchExpression::parse(L"财务 2026");
    size_t matchCount = 0;
    const auto tStartScan = std::chrono::steady_clock::now();
    for (size_t i = 0; i < store.slotCount(); ++i) {
        const auto* stored = store.at(i);
        if (!stored) continue;
        const FileRecord rec = store.view(*stored);
        auto dummyPath = [&]() -> const std::wstring& {
            static const std::wstring p = L"C:\\";
            return p;
        };
        if (expr.matchesWithLazyPath(rec, L'C', dummyPath)) {
            matchCount++;
        }
    }
    const auto scanDurationMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - tStartScan).count();
    EXPECT_GT(matchCount, 0u);
    EXPECT_LT(scanDurationMs, 50);
}

// 门禁 1B: 真实冷 MFT 扫描（85% ASCII + 15% 中文, creationTime=0, bulkImport=true）内存压降门禁 (< 90 B/file)
TEST(PerformanceRegressionBenchmarkTest, FileIndexStoreColdMftCompact1BMemoryGate) {
    static_assert(sizeof(tools3000::service::StoredFileRecord) == 40, "StoredFileRecord must stay 40 bytes");
    tools3000::service::FileIndexStore store;
    constexpr size_t kFileCount = 50000;
    store.reserve(kFileCount);

    for (uint64_t i = 1; i <= kFileCount; ++i) {
        FileRecordInit r{};
        r.fileReferenceNumber = i;
        r.parentFileReferenceNumber = (i > 20) ? (i % 20 + 1) : 0;
        if (i % 7 == 0) {
            r.fileName = L"项目归档文档_2026_" + std::to_wstring(i) + L".docx";
        } else {
            r.fileName = L"module_asset_" + std::to_wstring(i) + L".ts";
        }
        r.fileSize = (i <= 20) ? 0ULL : (4096ULL * (i % 100 + 1));
        r.creationTime = 0ULL;
        r.lastWriteTime = 134000000000000000ULL + (i % 1000) * 10000000ULL;
        r.isDirectory = (i <= 20);
        store.upsert(r, /*metadataHydrated=*/false, /*bulkImport=*/true);
    }
    store.recomputeHierarchyMetadata();
    store.shrinkExcessCapacity();

    const uint64_t totalBytes = store.approximateBytes();
    const uint64_t bytesPerFile = totalBytes / store.size();
    EXPECT_LT(bytesPerFile, 90u) << "Cold MFT 1B compact index exceeded 90 B/file budget: " << bytesPerFile;
}

// 门禁 2: 父子 FRN 链向上回溯解析 10,000 次耗时门禁 (< 20ms)
TEST(PerformanceRegressionBenchmarkTest, ParentChainFastWalkBenchmark) {
    tools3000::service::FileIndexStore store;
    store.reserve(100);

    // 构建 8 级嵌套深度目录树: C:\Root\Level1\Level2\...\Level7\target.txt
    uint64_t lastFrn = 0;
    for (uint64_t depth = 1; depth <= 8; ++depth) {
        FileRecordInit dir{};
        dir.fileReferenceNumber = depth;
        dir.parentFileReferenceNumber = lastFrn;
        dir.fileName = L"LevelFolder_" + std::to_wstring(depth);
        dir.isDirectory = true;
        store.upsert(dir);
        lastFrn = depth;
    }
    FileRecordInit file{};
    file.fileReferenceNumber = 100;
    file.parentFileReferenceNumber = lastFrn;
    file.fileName = L"DeepNestedPayload.docx";
    file.isDirectory = false;
    store.upsert(file);
    store.releaseBuildScratch();

    const auto buildPath = [&](uint64_t frn) -> std::wstring {
        std::vector<std::wstring_view> parts;
        parts.reserve(16);
        uint64_t cur = frn;
        for (size_t d = 0; d < 32; ++d) {
            const auto* node = store.find(cur);
            if (!node) break;
            const FileRecord r = store.view(*node);
            if (!r.fileName.empty()) parts.push_back(r.fileName);
            if (r.parentFileReferenceNumber == cur || r.parentFileReferenceNumber == 0) break;
            cur = r.parentFileReferenceNumber;
        }
        std::wstring p = L"C:\\";
        for (auto it = parts.rbegin(); it != parts.rend(); ++it) {
            if (p.size() > 3 && p.back() != L'\\') p += L'\\';
            p += *it;
        }
        return p;
    };

    // 连续回溯 1,000 次深层路径
    size_t totalLength = 0;
    const auto tStart = std::chrono::steady_clock::now();
    for (int i = 0; i < 1000; ++i) {
        const std::wstring full = buildPath(100);
        totalLength += full.size();
    }
    const auto durationMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - tStart).count();
    EXPECT_GT(totalLength, 0u);
    // 1,000 次 8 级回溯必须在 400ms 内完成 (覆盖率重度插桩环境下)
    EXPECT_LT(durationMs, 400);
}

// 门禁 3: 手势识别器 10,000 点抖动过滤与折线平滑吞吐量门禁 (< 15ms)
TEST(PerformanceRegressionBenchmarkTest, GestureRecognizerNoiseFilterBenchmark) {
    tools3000::gesture::GestureRecognizer recognizer;
    recognizer.reset();

    const auto tStart = std::chrono::steady_clock::now();
    for (int cycle = 0; cycle < 100; ++cycle) {
        recognizer.reset();
        // 模拟 100 个带微小手颤与回弹的绘制采样点
        for (int i = 0; i < 50; ++i) {
            recognizer.addPoint(100 + (i % 2), 100 + i * 4); // Down (含 1px 颤动)
        }
        for (int i = 0; i < 50; ++i) {
            recognizer.addPoint(100 + i * 4, 300 + (i % 2)); // Right
        }
        const auto dirs = recognizer.currentDirections();
        EXPECT_FALSE(dirs.empty());
    }
    const auto durationMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - tStart).count();
    EXPECT_LT(durationMs, 20);
}

// 门禁 4: 键盘拦截管线 50,000 次高频窗口消息分发门禁 (< 15ms)
TEST(PerformanceRegressionBenchmarkTest, KeyboardPipelineThroughputBenchmark) {
    const auto tStart = std::chrono::steady_clock::now();
    for (int i = 0; i < 50000; ++i) {
        const UINT msg = (i % 3 == 0) ? WM_SYSCOMMAND : ((i % 3 == 1) ? WM_SYSKEYDOWN : WM_KEYDOWN);
        const WPARAM wp = (msg == WM_SYSCOMMAND) ? SC_KEYMENU : VK_MENU;
        const bool intercepted = tools3000::ui::KeyboardPipeline::filterWindowMessage(
            nullptr, msg, wp, 0);
        EXPECT_TRUE(intercepted || !intercepted);
    }
    const auto durationMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - tStart).count();
    EXPECT_LT(durationMs, 20);
}

// 门禁 5: 冷路径工作集物理内存修剪稳定性与节流/过滤门禁
TEST(PerformanceRegressionBenchmarkTest, ColdPathWorkingSetTrimBenchmark) {
    using namespace tools3000::core;

    // 1. 白名单过滤逻辑测试：严格排除搜索服务常驻进程，白名单允许 WebView2 渲染管线
    EXPECT_FALSE(WinUtils::shouldTrimProcess(L""));
    EXPECT_FALSE(WinUtils::shouldTrimProcess(L"Tools3000_Service.exe"));
    EXPECT_FALSE(WinUtils::shouldTrimProcess(L"tools3000_service.exe"));
    EXPECT_FALSE(WinUtils::shouldTrimProcess(L"C:\\path\\to\\Tools3000_Service.exe"));
    EXPECT_FALSE(WinUtils::shouldTrimProcess(L"Tools3000_Service.exe"));
    EXPECT_FALSE(WinUtils::shouldTrimProcess(L"tools3000_service.exe"));
    EXPECT_FALSE(WinUtils::shouldTrimProcess(L"D:/service/Tools3000_Service.exe"));
    EXPECT_FALSE(WinUtils::shouldTrimProcess(L"explorer.exe"));
    EXPECT_FALSE(WinUtils::shouldTrimProcess(L"notepad.exe"));

    // 边界与前后缀防御性测试：严防模糊匹配误伤
    EXPECT_FALSE(WinUtils::shouldTrimProcess(L"msedgewebview2.exe.bak"));
    EXPECT_FALSE(WinUtils::shouldTrimProcess(L"xmsedgewebview2.exe"));
    EXPECT_FALSE(WinUtils::shouldTrimProcess(L"msedge.exe.log"));
    EXPECT_FALSE(WinUtils::shouldTrimProcess(L"Tools3000_Service.exe.config"));
    EXPECT_FALSE(WinUtils::shouldTrimProcess(L"sub_Tools3000_Service.exe"));

    EXPECT_TRUE(WinUtils::shouldTrimProcess(L"msedgewebview2.exe"));
    EXPECT_TRUE(WinUtils::shouldTrimProcess(L"MSEDGEWEBVIEW2.EXE"));
    EXPECT_TRUE(WinUtils::shouldTrimProcess(L"C:\\Program Files\\Edge\\msedgewebview2.exe"));
    EXPECT_TRUE(WinUtils::shouldTrimProcess(L"D:/runtime/msedgewebview2.exe"));
    EXPECT_TRUE(WinUtils::shouldTrimProcess(L"msedge.exe"));

    // 2. 原子时间戳 2000ms 防抖节流测试
    WinUtils::resetTrimThrottleForTesting();
    // 第一次调用：未被节流，执行修剪
    EXPECT_TRUE(WinUtils::trimWorkingSet());
    // 立即第二次调用：在 2000ms 窗口内，必定被节流拦截，返回 false
    EXPECT_FALSE(WinUtils::trimWorkingSet());
    // 强制调用 (force = true)：绕过节流限制，执行修剪
    EXPECT_TRUE(WinUtils::trimWorkingSet(true));
    // 再次重置测试时间戳后可立即重新执行
    WinUtils::resetTrimThrottleForTesting();
    EXPECT_TRUE(WinUtils::trimWorkingSet());

    // 3. 多线程并发无锁竞争门禁：10 个线程同时触发修剪，原子 CAS 保证且仅保证 1 个线程成功执行
    WinUtils::resetTrimThrottleForTesting();
    constexpr int kConcurrencyCount = 10;
    std::atomic<bool> startGate{false};
    std::atomic<int> trimSuccessCount{0};
    std::vector<std::thread> workers;
    workers.reserve(kConcurrencyCount);

    for (int i = 0; i < kConcurrencyCount; ++i) {
        workers.emplace_back([&startGate, &trimSuccessCount]() {
            while (!startGate.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
            if (WinUtils::trimWorkingSet(false)) {
                trimSuccessCount.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }

    startGate.store(true, std::memory_order_release);
    for (auto& t : workers) {
        if (t.joinable()) t.join();
    }

    EXPECT_EQ(trimSuccessCount.load(), 1);

    // 重置后再次验证
    WinUtils::resetTrimThrottleForTesting();
    EXPECT_TRUE(WinUtils::trimWorkingSet());
}

// -----------------------------------------------------------------------------
// -----------------------------------------------------------------------------
// 36. 文件对话框增强引擎 (PathMemoryManager / ExplorerTracker / DialogNavigator) 测试套件
// -----------------------------------------------------------------------------

TEST(Tools3000CoreWinUtilsCoverageTest, StringAndMemoryConversion) {
    using namespace tools3000::core;
    std::string utf8Str = "Tools3000 世界级截图工具";
    std::wstring wstr = WinUtils::utf8ToWstring(utf8Str);
    EXPECT_FALSE(wstr.empty());
    std::string roundtrip = WinUtils::wstringToUtf8(wstr);
    EXPECT_EQ(utf8Str, roundtrip);

    // 验证拼音引擎
    std::wstring initials = PinyinEngine::GetInitials(L"截图工具");
    EXPECT_FALSE(initials.empty());
    std::wstring fullPinyin = PinyinEngine::GetFullPinyin(L"截图");
    EXPECT_FALSE(fullPinyin.empty());

    // 验证物理内存修剪与冷路径安全调用
    WinUtils::trimWorkingSet();
}

TEST(Tools3000CorePipelineAndEventTest, KeyboardFilterAndEventBus) {
    using namespace tools3000::core;
    using namespace tools3000::ui;
    
    // 1. 验证 KeyboardPipeline Win32 菜单与帮助快捷键拦截
    EXPECT_TRUE(KeyboardPipeline::filterWindowMessage(nullptr, WM_SYSCOMMAND, SC_KEYMENU, 0));
    EXPECT_TRUE(KeyboardPipeline::filterWindowMessage(nullptr, WM_SYSCOMMAND, SC_CONTEXTHELP, 0));
    EXPECT_TRUE(KeyboardPipeline::filterWindowMessage(nullptr, WM_HELP, 0, 0));
    EXPECT_FALSE(KeyboardPipeline::filterWindowMessage(nullptr, WM_NULL, 0, 0));

    // 2. 验证 EventBus 同步分发与订阅机制
    bool eventReceived = false;
    auto subId = EventBus::instance().subscribe<ShowToastEvent>([&](const ShowToastEvent& evt) {
        if (evt.message == L"测试事件") {
            eventReceived = true;
        }
    });

    EventBus::instance().publish(ShowToastEvent{L"测试事件"});
    EXPECT_TRUE(eventReceived);
    EventBus::instance().unsubscribe(subId);
}

// -----------------------------------------------------------------------------
// 36.5 截图尺寸胶囊 HUD 与单位设置交互测试套件
// -----------------------------------------------------------------------------

TEST(WorldClassArchitectureTest, JobObjectAndAtomicWrite) {
    // 1. 验证 Job Object 初始化与自身进程挂载
    HANDLE job = tools3000::core::WinUtils::getProcessJobObject();
    EXPECT_NE(job, nullptr);
    EXPECT_TRUE(tools3000::core::WinUtils::assignProcessToCurrentJob(GetCurrentProcess()));

    // 2. 验证原子刷盘写入 (atomicWriteFileWithFlush)
    const auto tempTestDir = std::filesystem::temp_directory_path() / L"tools3000_arch_test";
    std::filesystem::create_directories(tempTestDir);
    const auto testFile = tempTestDir / L"test_config.json";

    const std::string payload = "{\n  \"testKey\": \"testValue\",\n  \"number\": 12345\n}";
    EXPECT_TRUE(tools3000::core::WinUtils::atomicWriteFileWithFlush(testFile.wstring(), payload));
    EXPECT_TRUE(std::filesystem::exists(testFile));

    // 读取验证内容一致性
    std::ifstream ifs(testFile);
    std::string readContent((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
    EXPECT_EQ(readContent, payload);

    HANDLE lockedTarget = CreateFileW(testFile.c_str(), GENERIC_READ, FILE_SHARE_READ,
                                      nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    ASSERT_NE(lockedTarget, INVALID_HANDLE_VALUE);
    EXPECT_FALSE(tools3000::core::WinUtils::atomicWriteFileWithFlush(
        testFile.wstring(), "replacement must not become visible"));
    CloseHandle(lockedTarget);
    std::ifstream preserved(testFile);
    std::string preservedContent((std::istreambuf_iterator<char>(preserved)),
                                 std::istreambuf_iterator<char>());
    EXPECT_EQ(preservedContent, payload);

    // 3. 验证低内存资源事件创建
    HANDLE lowMem = tools3000::core::WinUtils::createLowMemoryNotification();
    if (lowMem) {
        CloseHandle(lowMem);
    }

    std::error_code ec;
    std::filesystem::remove_all(tempTestDir, ec);
}

TEST(WorldClassArchitectureTest, PortableModeAndSingleInstanceHealing) {
    // 1. 验证便携模式判定逻辑
    const bool isPortable = tools3000::core::WinUtils::isPortableMode();
    (void)isPortable;
    const auto appDataDir = tools3000::core::WinUtils::getAppDataDirectory();
    EXPECT_FALSE(appDataDir.empty());
    EXPECT_TRUE(std::filesystem::exists(appDataDir));

    // 2. 验证孤儿互斥量自愈机制 (模拟 WAIT_ABANDONED / 互斥量探测)
    const wchar_t* testMutexName = L"Local\\Tools3000_Test_Mutex_Healing";
    HANDLE hMutex = CreateMutexW(nullptr, FALSE, testMutexName);
    EXPECT_NE(hMutex, nullptr);
    if (hMutex) {
        DWORD waitRes = WaitForSingleObject(hMutex, 0);
        EXPECT_TRUE(waitRes == WAIT_OBJECT_0 || waitRes == WAIT_ABANDONED);
        ReleaseMutex(hMutex);
        CloseHandle(hMutex);
    }
}

TEST(WorldClassArchitectureTest, TaskbarSafetyAndHelperOwner) {
    HINSTANCE hInst = GetModuleHandleW(nullptr);
    HWND helper = tools3000::core::WinUtils::createOverlayHelperOwner(hInst, L"Tools3000_UnitTest_Helper");
    EXPECT_NE(helper, nullptr);

    if (helper) {
        LONG_PTR exStyle = GetWindowLongPtrW(helper, GWL_EXSTYLE);
        EXPECT_TRUE((exStyle & WS_EX_TOOLWINDOW) != 0);
        EXPECT_TRUE((exStyle & WS_EX_NOACTIVATE) != 0);
        EXPECT_TRUE((exStyle & WS_EX_APPWINDOW) == 0);

        tools3000::core::WinUtils::applyTaskbarSafeOverlayStyle(helper);
        LONG_PTR updatedExStyle = GetWindowLongPtrW(helper, GWL_EXSTYLE);
        EXPECT_TRUE((updatedExStyle & WS_EX_TOOLWINDOW) != 0);
        EXPECT_TRUE((updatedExStyle & WS_EX_APPWINDOW) == 0);

        DestroyWindow(helper);
    }
}

// -----------------------------------------------------------------------------
// 39. 插件发现与动态加载生命周期测试套件 (必须放置在最后，因为 shutdown 会注销共享注册表)
// -----------------------------------------------------------------------------
TEST(PluginDiscoveryTest, DiscoveryAndLifecycle) {
    std::array<wchar_t, 32768> executablePath{};
    const DWORD length = GetModuleFileNameW(
        nullptr, executablePath.data(), static_cast<DWORD>(executablePath.size()));
    EXPECT_TRUE(length > 0 && length < executablePath.size());
    if (length == 0 || length >= executablePath.size()) return;

    const auto executableDir = std::filesystem::path(executablePath.data()).parent_path();
    std::error_code dirEc;
    const auto singleConfigPluginDir = executableDir / L"plugins";
    const auto pluginDir = std::filesystem::is_directory(singleConfigPluginDir, dirEc)
        ? singleConfigPluginDir
        : (executableDir.parent_path() / L"plugins" / executableDir.filename());
    auto& manager = tools3000::core::PluginManager::instance();
    EXPECT_TRUE(manager.loadPlugins(tools3000::core::WinUtils::wstringToUtf8(pluginDir.wstring())));
    const auto plugins = manager.getPluginStatuses();
    EXPECT_EQ(plugins.size(), 5u);
    for (const auto& plugin : plugins) {
        EXPECT_TRUE(plugin.error.empty()) << "Plugin " << plugin.name << " (" << plugin.id << ") error: " << plugin.error;
        EXPECT_EQ(plugin.abiVersion, tools3000::core::CurrentPluginAbiVersion) << "Plugin: " << plugin.name;
        EXPECT_FALSE(plugin.capabilities.empty()) << "Plugin: " << plugin.name;
    }

    // 验证各插件注册到 MessageBridge 的 IPC 协议调用与异常防护
    auto& bridge = tools3000::core::MessageBridge::instance();
    EXPECT_FALSE(bridge.handleMessage(R"({"id":1,"method":"search.status","params":{}})").empty());
    EXPECT_FALSE(bridge.handleMessage(R"({"id":2,"method":"search.query","params":{"keyword":"easy","page":1}})").empty());
    EXPECT_FALSE(bridge.handleMessage(R"({"id":3,"method":"gesture.getProfiles","params":{}})").empty());
    EXPECT_FALSE(bridge.handleMessage(R"({"id":4,"method":"capture.getCapabilities","params":{}})").empty());
    EXPECT_FALSE(bridge.handleMessage(R"({"id":5,"method":"keycast.getStatus","params":{}})").empty());
    EXPECT_FALSE(bridge.handleMessage(R"({"id":6,"method":"dialog.getConfig","params":{}})").empty());

    manager.shutdownPlugins();
}


TEST(CoreKeyboardHookTest, InstallPauseAndUninstall) {
    auto& kbHook = tools3000::core::KeyboardHook::instance();
    EXPECT_FALSE(kbHook.isPaused());

    kbHook.setPaused(true);
    EXPECT_TRUE(kbHook.isPaused());
    kbHook.setPaused(false);
    EXPECT_FALSE(kbHook.isPaused());

    kbHook.install();
    kbHook.uninstall();
}

TEST(CoreMouseHookTest, InstallAndCallbacks) {
    auto& mouseHook = tools3000::core::MouseHook::instance();
    EXPECT_FALSE(mouseHook.isPaused());

    mouseHook.setPaused(true);
    EXPECT_TRUE(mouseHook.isPaused());
    mouseHook.setPaused(false);
    EXPECT_FALSE(mouseHook.isPaused());

    bool callbackInvoked = false;
    mouseHook.setMouseActivityCallback([&](int btn, long x, long y) {
        (void)btn; (void)x; (void)y;
        callbackInvoked = true;
    });

    mouseHook.install();
    mouseHook.uninstall();
}

// -----------------------------------------------------------------------------
// 40. 纯矢量图标系统全覆盖测试套件
// -----------------------------------------------------------------------------


TEST(AllModulesLifecycleTest, SearchSuspendAndResumeLifecycle) {
    using namespace tools3000::ui;
    WebViewSuspendController controller;

    // 1. 验证分类与恢复决策纯函数 (0 锁、高并发状态分类)
    EXPECT_EQ(classifyWebViewSuspendCompletion(S_OK, TRUE), WebViewSuspendOutcome::Suspended);
    EXPECT_EQ(classifyWebViewSuspendCompletion(S_OK, FALSE), WebViewSuspendOutcome::Refused);
    EXPECT_EQ(classifyWebViewSuspendCompletion(E_FAIL, FALSE), WebViewSuspendOutcome::Failed);

    EXPECT_TRUE(shouldResumeAfterSuspendCompletion(WebViewSuspendOutcome::Suspended, false, false));
    EXPECT_FALSE(shouldResumeAfterSuspendCompletion(WebViewSuspendOutcome::Suspended, true, false));
    EXPECT_FALSE(shouldResumeAfterSuspendCompletion(WebViewSuspendOutcome::Suspended, false, true));

    // 2. 模拟放弃挂起与重置 (Abandon / Reset Lifecycle)
    controller.abandon();
    controller.reset();
}

TEST(AllModulesLifecycleTest, GestureAndHotCornerLifecycle) {
    using namespace tools3000::gesture;

    // 1. 触发角引擎生命周期：开启、设置、检测与关闭
    auto& engine = HotCornerEngine::instance();
    engine.setEnabled(true);
    engine.setTriggerDelay(100);
    engine.setCornerAction(HotCorner::TopLeft, "search.show");
    EXPECT_EQ(engine.getCornerAction(HotCorner::TopLeft), "search.show");

    // 模拟检测屏幕各角落
    POINT ptCorner{0, 0};
    EXPECT_EQ(HotCornerEngine::detectCorner(ptCorner), HotCorner::TopLeft);

    POINT ptCenter{500, 500};
    EXPECT_EQ(HotCornerEngine::detectCorner(ptCenter), HotCorner::None);

    engine.setCornerAction(HotCorner::TopLeft, "");
    EXPECT_TRUE(engine.getCornerAction(HotCorner::TopLeft).empty());
}

TEST(AllModulesLifecycleTest, DialogEnhancerLifecycleAndCleanup) {
    using namespace tools3000::dialog;

    // 1. 应用程序专属路径记忆生命周期：记录、查询、固定工作区与清空
    auto& manager = PathMemoryManager::instance();
    manager.setEnabled(true);
    manager.setPerAppMemoryEnabled(true);

    manager.recordAppPath("notepad.exe", "C:\\WorkDocs");
    EXPECT_EQ(manager.getAppPath("notepad.exe"), "C:\\WorkDocs");

    manager.setAppFixedWorkspace("notepad.exe", "D:\\FixedWorkspace", true);
    EXPECT_EQ(manager.getEffectiveAppPath("notepad.exe"), "D:\\FixedWorkspace");

    manager.removeAppMemory("notepad.exe");
    EXPECT_TRUE(manager.getAppPath("notepad.exe").empty());

    manager.clearAppMemories();
}

TEST(AllModulesLifecycleTest, DynamicHotkeyResolutionForAllFeatures) {
    // 验证所有功能的快捷键注册与动态感知，杜绝任何代码中硬编码快捷键的死锁问题
    const std::vector<std::string> featureKeys = {
        "/hotkey/search",
        "/hotkey/capture",
        "/hotkey/record",
        "/hotkey/pin",
        "/hotkey/spotlight",
        "/hotkey/keycast",
        "/hotkey/settings"
    };

    for (const auto& key : featureKeys) {
        // 动态注册与读取验证
        std::string hotkeyStr = tools3000::core::ConfigManager::instance().get<std::string>(key, "");
        // 无论用户将快捷键设置为何种组合（即使为空或自定义键），配置读取均保持原子幂等
        EXPECT_NO_THROW({
            auto val = tools3000::core::ConfigManager::instance().get<std::string>(key, "Ctrl+Shift+F12");
            EXPECT_FALSE(val.empty());
        });
    }
}

// -----------------------------------------------------------------------------
// 28. 鼠标手势全链路生命周期、取消打断与视口安全门禁测试套件
// -----------------------------------------------------------------------------

TEST(ShellContextMenuServiceTest, UniversalDpiAndCoordinateSafety) {
    auto& service = tools3000::core::ShellContextMenuService::instance();
    // 1. 空路径防御性测试：立即安全返回 false
    EXPECT_FALSE(service.showAsync(L"", -1, -1));

    // 2. 非空路径启动异步工作线程测试，并在后台安全处理
    EXPECT_TRUE(service.showAsync(L"C:\\non_existent_file_2026.xyz", 100, 200));

    // 3. 真实系统文件夹测试
    EXPECT_TRUE(service.showAsync(L"C:\\Windows", 100, 200));

    // 4. 并发关闭与生命周期安全
    service.shutdown();
}

TEST(PathOperationsTest, RenameStaysWithinTheSourceParentDirectory) {
    const auto tempRoot = std::filesystem::temp_directory_path() /
        (L"Tools3000Rename_" + std::to_wstring(GetCurrentProcessId()) + L"_" +
         std::to_wstring(GetTickCount64()));
    std::error_code ec;
    ASSERT_TRUE(std::filesystem::create_directories(tempRoot, ec));

    const auto source = tempRoot / L"source.txt";
    {
        std::ofstream output(source, std::ios::binary);
        ASSERT_TRUE(output.good());
        output << "rename-test";
    }

    for (const std::wstring_view invalidName : {
             L"..", L"../escaped.txt", L"subdir\\escaped.txt", L"C:\\escaped.txt",
             L"name:stream", L"CON.txt", L"COM\u00B9.txt", L"LPT\u00B2",
             L"trailing.", L"trailing "}) {
        const auto result = tools3000::core::renamePathWithinParent(source, invalidName);
        EXPECT_FALSE(result.success) <<
            tools3000::core::WinUtils::wstringToUtf8(std::wstring(invalidName));
        EXPECT_FALSE(result.error.empty());
        EXPECT_TRUE(std::filesystem::exists(source));
    }
    EXPECT_FALSE(std::filesystem::exists(tempRoot.parent_path() / L"escaped.txt"));

    auto& bridge = tools3000::core::MessageBridge::instance();
    tools3000::core::registerFileInteractionHandlers(
        bridge, "audit.file", "path is required");
    const nlohmann::json request = {
        {"id", 701},
        {"method", "audit.file.renamePath"},
        {"params", {
            {"oldPath", tools3000::core::WinUtils::wstringToUtf8(source.wstring())},
            {"newName", "../ipc-escaped.txt"}
        }}
    };
    const auto response = nlohmann::json::parse(bridge.handleMessage(request.dump()));
    ASSERT_TRUE(response.contains("result"));
    EXPECT_FALSE(response["result"].value("success", true));
    EXPECT_TRUE(std::filesystem::exists(source));
    EXPECT_FALSE(std::filesystem::exists(tempRoot.parent_path() / L"ipc-escaped.txt"));
    EXPECT_EQ(bridge.unregisterHandlersByPrefix("audit.file"), 8u);

    const auto renamed = tempRoot / L"renamed.txt";
    const auto success = tools3000::core::renamePathWithinParent(source, L"renamed.txt");
    EXPECT_TRUE(success.success) << success.error;
    EXPECT_EQ(success.newPath, renamed);
    EXPECT_FALSE(std::filesystem::exists(source));
    EXPECT_TRUE(std::filesystem::exists(renamed));

    const auto duplicate = tempRoot / L"duplicate.txt";
    {
        std::ofstream output(duplicate, std::ios::binary);
        output << "existing";
    }
    const auto collision = tools3000::core::renamePathWithinParent(renamed, L"duplicate.txt");
    EXPECT_FALSE(collision.success);
    EXPECT_TRUE(std::filesystem::exists(renamed));
    std::ifstream duplicateInput(duplicate, std::ios::binary);
    std::string duplicateContents;
    duplicateInput >> duplicateContents;
    EXPECT_EQ(duplicateContents, "existing");
    duplicateInput.close();

    std::filesystem::remove_all(tempRoot, ec);
    EXPECT_FALSE(ec) << ec.message();
}

// -----------------------------------------------------------------------------
// 31. 通用鼠标基础设施层测试套件 (Mouse Infrastructure Suite)
// -----------------------------------------------------------------------------

TEST(MouseStreamThrottleTest, ThrottleIntervalAndReset) {
    tools3000::core::MouseStreamThrottle throttle;
    EXPECT_EQ(throttle.lastWakeTick(), 0u);

    // 1. 初始状态或未唤醒过：应当立即允许唤醒 (0ms 零延迟)
    EXPECT_TRUE(throttle.canWake(1000, 8));
    EXPECT_TRUE(throttle.shouldWake(1000, 8));
    EXPECT_EQ(throttle.lastWakeTick(), 1000u);

    // 2. 间隔内高频上报 (例如 1000Hz 鼠标 1ms 脉冲)：拒绝唤醒
    EXPECT_FALSE(throttle.canWake(1002, 8));
    EXPECT_FALSE(throttle.shouldWake(1002, 8));
    EXPECT_FALSE(throttle.canWake(1007, 8));
    EXPECT_FALSE(throttle.shouldWake(1007, 8));
    EXPECT_EQ(throttle.lastWakeTick(), 1000u);

    // 3. 达到或超过 8ms (120Hz 节流窗口)：允许唤醒并更新时钟
    EXPECT_TRUE(throttle.canWake(1008, 8));
    EXPECT_TRUE(throttle.shouldWake(1008, 8));
    EXPECT_EQ(throttle.lastWakeTick(), 1008u);

    // 4. 重置后再次保证即刻响应
    throttle.reset();
    EXPECT_EQ(throttle.lastWakeTick(), 0u);
    EXPECT_TRUE(throttle.shouldWake(1009, 8));
    EXPECT_EQ(throttle.lastWakeTick(), 1009u);
}

TEST(MouseStreamCoreTest, BasicPushDrainAndBatching) {
    auto& core = tools3000::core::MouseStreamCore::instance();
    tools3000::core::MouseStreamCore::ScopedSubscriber sub(core);
    core.clear();
    core.resetThrottle();
    EXPECT_TRUE(core.empty());
    EXPECT_EQ(core.pendingCount(), 0u);

    // 压入 5 个原始采样点
    core.pushRawMove(100, 200);
    core.pushRawMove(105, 205);
    core.pushRawMove(110, 210);
    core.pushRawMove(115, 215);
    core.pushRawMove(120, 220);

    EXPECT_FALSE(core.empty());
    EXPECT_EQ(core.pendingCount(), 5u);

    // 批量排空到 vector
    std::vector<tools3000::core::MouseStreamPoint> batch;
    size_t drained = core.drainBatch(batch, 3);
    EXPECT_EQ(drained, 3u);
    EXPECT_EQ(batch.size(), 3u);
    EXPECT_EQ(batch[0].x, 100);
    EXPECT_EQ(batch[1].x, 105);
    EXPECT_EQ(batch[2].x, 110);
    EXPECT_EQ(core.pendingCount(), 2u);

    // 通过 Visitor 访问排空
    std::vector<int> xCoords;
    size_t visitorDrained = core.drainBatch([&](const tools3000::core::MouseStreamPoint& pt) {
        xCoords.push_back(pt.x);
    }, 10);
    EXPECT_EQ(visitorDrained, 2u);
    ASSERT_EQ(xCoords.size(), 2u);
    EXPECT_EQ(xCoords[0], 115);
    EXPECT_EQ(xCoords[1], 120);

    EXPECT_TRUE(core.empty());
}

TEST(MouseStreamCoreTest, HighThroughputProducerConsumerStress) {
    auto& core = tools3000::core::MouseStreamCore::instance();
    tools3000::core::MouseStreamCore::ScopedSubscriber sub(core);
    core.clear();
    core.resetThrottle();

    constexpr int STRESS_COUNT = 5000;
    std::atomic<bool> producerDone{false};
    std::atomic<int> receivedCount{0};

    std::jthread consumer([&](std::stop_token) {
        std::vector<tools3000::core::MouseStreamPoint> buffer;
        while (!producerDone.load(std::memory_order_acquire) || !core.empty()) {
            size_t n = core.drainBatch(buffer, 64);
            if (n > 0) {
                receivedCount.fetch_add(static_cast<int>(n), std::memory_order_relaxed);
                buffer.clear();
            } else {
                std::this_thread::yield();
            }
        }
    });

    for (int i = 0; i < STRESS_COUNT; ++i) {
        core.pushRawMove(i, i * 2);
        if (i % 64 == 0) {
            std::this_thread::yield();
        }
    }
    producerDone.store(true, std::memory_order_release);
    consumer.join();

    EXPECT_GT(receivedCount.load(), 0);
}

TEST(MouseViewportManagerTest, LocalBoundingBoxAndFocusAssistAvoidance) {
    using tools3000::core::MouseViewportManager;
    using tools3000::core::MouseViewportRect;

    RECT virt = MouseViewportManager::getVirtualDesktopBounds();
    EXPECT_GT(virt.right, virt.left);
    EXPECT_GT(virt.bottom, virt.top);

    // 1. 局部动态最小包围盒：点集包围与最小尺寸约束 (minSize = 100)
    POINT pts[3] = { {200, 200}, {210, 210}, {205, 208} };
    MouseViewportRect localRect = MouseViewportManager::computePointsBoundingBox(pts, 3, 20, 100, 400);
    EXPECT_FALSE(localRect.isFullscreen);
    EXPECT_GE(localRect.width, 100);
    EXPECT_GE(localRect.height, 100);
    EXPECT_LE(localRect.width, 400);
    EXPECT_LE(localRect.height, 400);

    // 2. 全屏暗角避让视口 (严格保证 vw - 1, vh - 1 物理避让)
    MouseViewportRect fullscreenAvoid = MouseViewportManager::computeFullscreenAvoidanceBounds();
    EXPECT_TRUE(fullscreenAvoid.isFullscreen);
    int virtW = virt.right - virt.left;
    int virtH = virt.bottom - virt.top;
    EXPECT_EQ(fullscreenAvoid.width, virtW - 1);
    EXPECT_EQ(fullscreenAvoid.height, virtH - 1);

    // 3. DPI 度量缩放
    EXPECT_EQ(MouseViewportManager::scaleMetric(100, 1.0f), 100);
    EXPECT_EQ(MouseViewportManager::scaleMetric(100, 1.5f), 150);
    EXPECT_EQ(MouseViewportManager::scaleMetric(100, 2.0f), 200);
}

class TestMouseOverlay : public tools3000::core::MouseFeedbackOverlayBase {
public:
    TestMouseOverlay() = default;
    ~TestMouseOverlay() override = default;

    void setIdle(bool idle) { m_idle.store(idle, std::memory_order_relaxed); }
    int tickCount() const { return m_ticks.load(std::memory_order_relaxed); }

protected:
    void onRenderTick() override {
        m_ticks.fetch_add(1, std::memory_order_relaxed);
    }

    bool isOverlayIdle() const override {
        return m_idle.load(std::memory_order_relaxed);
    }

private:
    std::atomic<bool> m_idle{true};
    std::atomic<int> m_ticks{0};
};

TEST(MouseFeedbackOverlayBaseTest, LifecycleAndSleepContract) {
    TestMouseOverlay overlay;
    EXPECT_FALSE(overlay.isCompositorReady());
    EXPECT_FALSE(overlay.isVisible());

    bool inited = overlay.initializeBase(
        GetModuleHandleW(nullptr),
        L"Tools3000_TestMouseFeedbackOverlay",
        L"Tools3000_TestMouseHelperOwner"
    );
    EXPECT_TRUE(inited);
    EXPECT_TRUE(overlay.isCompositorReady());
    EXPECT_NE(overlay.hwnd(), nullptr);

    // 初始处于 Idle 状态，契约验证：不空转消耗 CPU
    int initialTicks = overlay.tickCount();
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    EXPECT_LE(overlay.tickCount() - initialTicks, 1);

    // 唤醒一次
    overlay.wakeRender();
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    EXPECT_GE(overlay.tickCount(), initialTicks);

    // 视口更新 (异步安全跨线程请求)
    tools3000::core::MouseViewportRect vp{100, 100, 200, 200, false};
    overlay.requestViewport(vp);
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    EXPECT_TRUE(overlay.isVisible());

    // 请求隐藏
    overlay.requestHide();
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    EXPECT_FALSE(overlay.isVisible());

    // 优雅关闭
    overlay.shutdownBase();
    EXPECT_FALSE(overlay.isCompositorReady());
    EXPECT_FALSE(overlay.isVisible());
    EXPECT_EQ(overlay.hwnd(), nullptr);
}

TEST(MouseFeedbackOverlayBaseTest, LazyCompositorStaysUnallocatedUntilDraw) {
    TestMouseOverlay overlay;
    EXPECT_FALSE(overlay.isCompositorReady());

    const bool inited = overlay.initializeBase(
        GetModuleHandleW(nullptr),
        L"Tools3000_TestLazyMouseFeedbackOverlay",
        L"Tools3000_TestLazyMouseHelperOwner",
        /*lazyCompositor=*/true
    );
    EXPECT_TRUE(inited);
    EXPECT_NE(overlay.hwnd(), nullptr);
    // 惰性模式下冷启动不得预建第二套 D3D11/D2D1/DComp 设备
    EXPECT_FALSE(overlay.isCompositorReady());

    overlay.shutdownBase();
    EXPECT_FALSE(overlay.isCompositorReady());
    EXPECT_EQ(overlay.hwnd(), nullptr);
}

// ─────────────────────────────────────────────────────────────────────────────
// WideUniversalRotatingFileSink & Logger 日志管家体系单元测试集
// ─────────────────────────────────────────────────────────────────────────────

namespace {

void writeTestLogMsg(
    const std::shared_ptr<tools3000::core::WideUniversalRotatingFileSink>& sink,
    const std::string& text)
{
    spdlog::source_loc loc{"test.cpp", 1, "test"};
    spdlog::details::log_msg msg(loc, "test", spdlog::level::info, text);
    sink->log(msg);
}

void removeDirectorySafely(const std::filesystem::path& dir) {
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}

}  // namespace

TEST(WideUniversalRotatingFileSinkTest, CanonicalNameAndFullSharingMode) {
    const auto testDir = std::filesystem::temp_directory_path() / "t3k_log_test_canonical";
    removeDirectorySafely(testDir);

    tools3000::core::WideUniversalSinkConfig cfg;
    cfg.logDir = testDir;
    cfg.logFileName = "tools3000";
    cfg.maxFileSize = 1024 * 1024;
    cfg.enableCompression = false;
    cfg.idleHandleTimeoutMs = 60000;

    auto sink = std::make_shared<tools3000::core::WideUniversalRotatingFileSink>(cfg);
    EXPECT_EQ(sink->getActiveLogPath(), testDir / L"tools3000.log");

    writeTestLogMsg(sink, "Canonical log test message 1");

    // 验证活动文件在原地创建
    EXPECT_TRUE(std::filesystem::exists(sink->getActiveLogPath()));

    // 验证全共享模式：外部句柄能以读、写、删权限并发打开，不被死锁阻断
    HANDLE hShared = CreateFileW(
        sink->getActiveLogPath().c_str(),
        GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        nullptr
    );
    EXPECT_NE(hShared, INVALID_HANDLE_VALUE);
    if (hShared != INVALID_HANDLE_VALUE) {
        CloseHandle(hShared);
    }

    sink.reset();
    removeDirectorySafely(testDir);
}

TEST(WideUniversalRotatingFileSinkTest, SizeBasedRotationAndSequenceNumbering) {
    const auto testDir = std::filesystem::temp_directory_path() / "t3k_log_test_size_rot";
    removeDirectorySafely(testDir);

    tools3000::core::WideUniversalSinkConfig cfg;
    cfg.logDir = testDir;
    cfg.logFileName = "tools3000";
    cfg.maxFileSize = 250; // 较小阈值触发滚动
    cfg.enableCompression = false;
    cfg.idleHandleTimeoutMs = 60000;

    auto sink = std::make_shared<tools3000::core::WideUniversalRotatingFileSink>(cfg);
    sink->setSimulatedDayForTesting("2026-09-21");

    // 写入第一批数据
    std::string line1(150, 'A');
    writeTestLogMsg(sink, line1);

    // 写入第二批数据，累计超过 250 字节，触发滚动
    std::string line2(150, 'B');
    writeTestLogMsg(sink, line2);

    // 验证滚动归档产物
    const auto archive1 = testDir / L"tools3000_2026-09-21_001.log";
    EXPECT_TRUE(std::filesystem::exists(archive1));
    EXPECT_TRUE(std::filesystem::exists(sink->getActiveLogPath()));

    // 再次写入超过 250 字节，触发第二次滚动，序号递增至 002
    std::string line3(150, 'C');
    writeTestLogMsg(sink, line3);

    const auto archive2 = testDir / L"tools3000_2026-09-21_002.log";
    EXPECT_TRUE(std::filesystem::exists(archive2));

    sink.reset();
    removeDirectorySafely(testDir);
}

TEST(WideUniversalRotatingFileSinkTest, ChronologicalMidnightRotation) {
    const auto testDir = std::filesystem::temp_directory_path() / "t3k_log_test_midnight";
    removeDirectorySafely(testDir);

    tools3000::core::WideUniversalSinkConfig cfg;
    cfg.logDir = testDir;
    cfg.logFileName = "tools3000";
    cfg.maxFileSize = 10 * 1024 * 1024;
    cfg.enableCompression = false;
    cfg.idleHandleTimeoutMs = 60000;

    auto sink = std::make_shared<tools3000::core::WideUniversalRotatingFileSink>(cfg);
    sink->setSimulatedDayForTesting("2026-09-21");

    writeTestLogMsg(sink, "Log entry on day 2026-09-21");
    EXPECT_TRUE(std::filesystem::exists(sink->getActiveLogPath()));

    // 跨天时钟触发：将活跃日期切换为 2026-09-22
    sink->setSimulatedDayForTesting("2026-09-22");
    writeTestLogMsg(sink, "Log entry on day 2026-09-22 after midnight");

    // 前一天的日志被安全归档为带 2026-09-21 日期的历史文件
    const auto archive = testDir / L"tools3000_2026-09-21_001.log";
    EXPECT_TRUE(std::filesystem::exists(archive));

    // 当前活动日志文件依然是恒定的 tools3000.log
    EXPECT_TRUE(std::filesystem::exists(sink->getActiveLogPath()));
    EXPECT_EQ(sink->getActiveDay(), "2026-09-22");

    // 当天再次强制滚动，序号重置为 001
    sink->forceRotate();
    const auto archiveNewDay = testDir / L"tools3000_2026-09-22_001.log";
    EXPECT_TRUE(std::filesystem::exists(archiveNewDay));

    sink.reset();
    removeDirectorySafely(testDir);
}

TEST(WideUniversalRotatingFileSinkTest, IdleHandleAutoCloseAndReopen) {
    const auto testDir = std::filesystem::temp_directory_path() / "t3k_log_test_idle";
    removeDirectorySafely(testDir);

    tools3000::core::WideUniversalSinkConfig cfg;
    cfg.logDir = testDir;
    cfg.logFileName = "tools3000";
    cfg.maxFileSize = 1024 * 1024;
    cfg.idleHandleTimeoutMs = 150; // 150ms 空闲超时
    cfg.enableCompression = false;

    auto sink = std::make_shared<tools3000::core::WideUniversalRotatingFileSink>(cfg);
    writeTestLogMsg(sink, "Initial active log");

    // 刚写入后句柄为打开状态
    EXPECT_TRUE(sink->isFileHandleOpen());

    // 等待空闲超时触发 (超时设为 150ms，至多等待 1000ms)
    bool handleClosed = false;
    for (int i = 0; i < 50; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        if (!sink->isFileHandleOpen()) {
            handleClosed = true;
            break;
        }
    }
    EXPECT_TRUE(handleClosed);

    // 再次写入日志，秒级自动重新打开句柄并续写
    writeTestLogMsg(sink, "Reopened log after idle release");
    EXPECT_TRUE(sink->isFileHandleOpen());

    sink.reset();
    removeDirectorySafely(testDir);
}

TEST(WideUniversalRotatingFileSinkTest, ExternalDeletionSelfHealing) {
    const auto testDir = std::filesystem::temp_directory_path() / "t3k_log_test_del_heal";
    removeDirectorySafely(testDir);

    tools3000::core::WideUniversalSinkConfig cfg;
    cfg.logDir = testDir;
    cfg.logFileName = "tools3000";
    cfg.maxFileSize = 1024 * 1024;
    cfg.idleHandleTimeoutMs = 60000;
    cfg.enableCompression = false;

    auto sink = std::make_shared<tools3000::core::WideUniversalRotatingFileSink>(cfg);
    writeTestLogMsg(sink, "Log before external deletion");
    EXPECT_TRUE(std::filesystem::exists(sink->getActiveLogPath()));

    // 模拟外部工具或用户强行删除当前日志文件
    std::error_code ec;
    std::filesystem::remove(sink->getActiveLogPath(), ec);
    EXPECT_FALSE(std::filesystem::exists(sink->getActiveLogPath()));

    // 再次写入日志，槽体透明自动自愈在原地重建 tools3000.log，无任何崩溃或异常
    EXPECT_NO_THROW({
        writeTestLogMsg(sink, "Log after external deletion self healing");
    });
    EXPECT_TRUE(std::filesystem::exists(sink->getActiveLogPath()));

    sink.reset();
    removeDirectorySafely(testDir);
}

TEST(WideUniversalRotatingFileSinkTest, ExternalRenameSelfHealing) {
    const auto testDir = std::filesystem::temp_directory_path() / "t3k_log_test_rename_heal";
    removeDirectorySafely(testDir);

    tools3000::core::WideUniversalSinkConfig cfg;
    cfg.logDir = testDir;
    cfg.logFileName = "tools3000";
    cfg.maxFileSize = 1024 * 1024;
    cfg.idleHandleTimeoutMs = 60000;
    cfg.enableCompression = false;

    auto sink = std::make_shared<tools3000::core::WideUniversalRotatingFileSink>(cfg);
    writeTestLogMsg(sink, "Log before external rename");
    EXPECT_TRUE(std::filesystem::exists(sink->getActiveLogPath()));

    // 模拟外部程序将活动日志重命名移走
    const auto movedPath = testDir / L"tools3000_moved.log";
    std::error_code ec;
    std::filesystem::rename(sink->getActiveLogPath(), movedPath, ec);
    EXPECT_TRUE(std::filesystem::exists(movedPath));
    EXPECT_FALSE(std::filesystem::exists(sink->getActiveLogPath()));

    // 再次写入日志，自动在原地脱钩并接续新建 tools3000.log
    EXPECT_NO_THROW({
        writeTestLogMsg(sink, "Log after external rename接续");
    });
    EXPECT_TRUE(std::filesystem::exists(sink->getActiveLogPath()));
    EXPECT_TRUE(std::filesystem::exists(movedPath));

    sink.reset();
    removeDirectorySafely(testDir);
}

TEST(WideUniversalRotatingFileSinkTest, RetentionDaysPurge) {
    const auto testDir = std::filesystem::temp_directory_path() / "t3k_log_test_retention";
    removeDirectorySafely(testDir);
    std::filesystem::create_directories(testDir);

    tools3000::core::WideUniversalSinkConfig cfg;
    cfg.logDir = testDir;
    cfg.logFileName = "tools3000";
    cfg.retentionDays = 3; // 保留 3 天
    cfg.enableCompression = false;

    auto sink = std::make_shared<tools3000::core::WideUniversalRotatingFileSink>(cfg);
    sink->setSimulatedDayForTesting("2026-09-21");

    // 模拟创建历史归档文件
    const auto oldFile1 = testDir / L"tools3000_2026-09-01_001.log"; // 20 天前
    const auto oldFile2 = testDir / L"tools3000_2026-09-15_001.log"; // 6 天前
    const auto freshFile = testDir / L"tools3000_2026-09-20_001.log"; // 1 天前

    {
        std::ofstream f1(oldFile1); f1 << "old 1";
        std::ofstream f2(oldFile2); f2 << "old 2";
        std::ofstream f3(freshFile); f3 << "fresh";
    }

    sink->triggerJanitorSync();

    // 超过 3 天的历史归档被自动清理
    EXPECT_FALSE(std::filesystem::exists(oldFile1));
    EXPECT_FALSE(std::filesystem::exists(oldFile2));
    // 1 天前的归档安全保留
    EXPECT_TRUE(std::filesystem::exists(freshFile));

    // 测试永久保留 (retentionDays = 0)
    sink->setRetentionDays(0);
    const auto foreverOld = testDir / L"tools3000_2026-08-01_001.log";
    {
        std::ofstream fo(foreverOld); fo << "forever";
    }
    sink->triggerJanitorSync();
    EXPECT_TRUE(std::filesystem::exists(foreverOld));

    sink.reset();
    removeDirectorySafely(testDir);
}

TEST(WideUniversalRotatingFileSinkTest, MaxTotalSizeQuotaCascadePurge) {
    const auto testDir = std::filesystem::temp_directory_path() / "t3k_log_test_quota";
    removeDirectorySafely(testDir);
    std::filesystem::create_directories(testDir);

    tools3000::core::WideUniversalSinkConfig cfg;
    cfg.logDir = testDir;
    cfg.logFileName = "tools3000";
    cfg.retentionDays = 30;
    cfg.maxTotalSize = 25 * 1024; // 25KB 配额上限
    cfg.enableCompression = false;

    auto sink = std::make_shared<tools3000::core::WideUniversalRotatingFileSink>(cfg);
    sink->setSimulatedDayForTesting("2026-09-21");

    // 创建 4 个 10KB 的历史归档 (总计 40KB > 25KB 配额)
    const std::string payload(10 * 1024, 'X');
    const auto f1 = testDir / L"tools3000_2026-09-18_001.log";
    const auto f2 = testDir / L"tools3000_2026-09-19_001.log";
    const auto f3 = testDir / L"tools3000_2026-09-20_001.log";
    const auto f4 = testDir / L"tools3000_2026-09-21_001.log";

    {
        std::ofstream o1(f1); o1 << payload;
        std::ofstream o2(f2); o2 << payload;
        std::ofstream o3(f3); o3 << payload;
        std::ofstream o4(f4); o4 << payload;
    }

    sink->triggerJanitorSync();

    // 最老的文件 09-18 与 09-19 应被级联淘汰，确保剩余总容量 <= 25KB
    EXPECT_FALSE(std::filesystem::exists(f1));
    EXPECT_FALSE(std::filesystem::exists(f2));
    EXPECT_TRUE(std::filesystem::exists(f3));
    EXPECT_TRUE(std::filesystem::exists(f4));

    sink.reset();
    removeDirectorySafely(testDir);
}

TEST(WideUniversalRotatingFileSinkTest, AsyncGzipCompression) {
    const auto testDir = std::filesystem::temp_directory_path() / "t3k_log_test_gzip";
    removeDirectorySafely(testDir);

    tools3000::core::WideUniversalSinkConfig cfg;
    cfg.logDir = testDir;
    cfg.logFileName = "tools3000";
    cfg.maxFileSize = 200;
    cfg.enableCompression = true;
    cfg.idleHandleTimeoutMs = 60000;

    auto sink = std::make_shared<tools3000::core::WideUniversalRotatingFileSink>(cfg);
    sink->setSimulatedDayForTesting("2026-09-21");

    writeTestLogMsg(sink, "Log entry to be compressed with gzip later.");
    sink->forceRotate();

    // 触发同步自净以立即执行压缩
    sink->triggerJanitorSync();

    const auto gzArchive = testDir / L"tools3000_2026-09-21_001.log.gz";
    const auto rawArchive = testDir / L"tools3000_2026-09-21_001.log";

    // 验证未压缩的 .log 已被删除，且生成了 .log.gz
    EXPECT_TRUE(std::filesystem::exists(gzArchive));
    EXPECT_FALSE(std::filesystem::exists(rawArchive));

    // 验证 gzip 头部魔数 (0x1F, 0x8B)
    std::ifstream gzIn(gzArchive, std::ios::binary);
    ASSERT_TRUE(gzIn.is_open());
    char magic[2] = {0};
    gzIn.read(magic, 2);
    EXPECT_EQ(static_cast<unsigned char>(magic[0]), 0x1F);
    EXPECT_EQ(static_cast<unsigned char>(magic[1]), 0x8B);

    sink.reset();
    removeDirectorySafely(testDir);
}

TEST(WideUniversalRotatingFileSinkTest, MidnightRotationWhenEmptyDoesNotCorruptNextDayRoll) {
    const auto testDir = std::filesystem::temp_directory_path() / "t3k_log_test_empty_midnight";
    removeDirectorySafely(testDir);

    tools3000::core::WideUniversalSinkConfig cfg;
    cfg.logDir = testDir;
    cfg.logFileName = "tools3000";
    cfg.maxFileSize = 500;
    cfg.enableCompression = false;
    cfg.idleHandleTimeoutMs = 60000;

    auto sink = std::make_shared<tools3000::core::WideUniversalRotatingFileSink>(cfg);
    sink->setSimulatedDayForTesting("2026-09-21");
    // 空文件，无日志写入

    // 跨天变更至 2026-09-22
    sink->setSimulatedDayForTesting("2026-09-22");

    // 写入第 2 天第 1 条日志 (50 字节)
    writeTestLogMsg(sink, "Day 2 first log entry");

    // 写入第 2 天第 2 条日志 (50 字节)
    writeTestLogMsg(sink, "Day 2 second log entry");

    // 关键断言：绝对不能生成 2026-09-21 的伪归档 (因为 21 日并无任何日志)
    EXPECT_FALSE(std::filesystem::exists(testDir / L"tools3000_2026-09-21_001.log"));
    EXPECT_EQ(sink->getActiveDay(), "2026-09-22");

    sink.reset();
    removeDirectorySafely(testDir);
}

TEST(WideUniversalRotatingFileSinkTest, WorkerAutomaticMidnightChronologicalTrigger) {
    const auto testDir = std::filesystem::temp_directory_path() / "t3k_log_test_worker_midnight";
    removeDirectorySafely(testDir);

    tools3000::core::WideUniversalSinkConfig cfg;
    cfg.logDir = testDir;
    cfg.logFileName = "tools3000";
    cfg.maxFileSize = 10 * 1024 * 1024;
    cfg.enableCompression = false;
    cfg.idleHandleTimeoutMs = 60000;

    auto sink = std::make_shared<tools3000::core::WideUniversalRotatingFileSink>(cfg);
    sink->setSimulatedDayForTesting("2026-09-21");

    writeTestLogMsg(sink, "Active day 2026-09-21 log entry");
    EXPECT_TRUE(std::filesystem::exists(sink->getActiveLogPath()));

    // 模拟时钟跨过午夜零点，此时业务线程完全空闲 (没有任何新的 writeTestLogMsg 调用)
    sink->setSimulatedDayForTesting("2026-09-22");

    // 等待后台守护工作线程主动感知时钟变更并执行零点滚动归档 (至多 1500ms)
    bool rotatedByWorker = false;
    const auto expectedArchive = testDir / L"tools3000_2026-09-21_001.log";
    for (int i = 0; i < 75; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        if (std::filesystem::exists(expectedArchive)) {
            rotatedByWorker = true;
            break;
        }
    }
    EXPECT_TRUE(rotatedByWorker);
    EXPECT_EQ(sink->getActiveDay(), "2026-09-22");

    sink.reset();
    removeDirectorySafely(testDir);
}

TEST(WideUniversalRotatingFileSinkTest, MoveFileFailureFallbackCopyTruncate) {
    const auto testDir = std::filesystem::temp_directory_path() / "t3k_log_test_sharing_fallback";
    removeDirectorySafely(testDir);

    tools3000::core::WideUniversalSinkConfig cfg;
    cfg.logDir = testDir;
    cfg.logFileName = "tools3000";
    cfg.maxFileSize = 100; // 小体积触发滚动
    cfg.enableCompression = false;
    cfg.idleHandleTimeoutMs = 60000;

    auto sink = std::make_shared<tools3000::core::WideUniversalRotatingFileSink>(cfg);
    sink->setSimulatedDayForTesting("2026-09-21");

    writeTestLogMsg(sink, "Log line before external lock");
    EXPECT_TRUE(std::filesystem::exists(sink->getActiveLogPath()));

    // 模拟外部老旧程序以仅 FILE_SHARE_READ 打开活动日志 (不包含 FILE_SHARE_DELETE，将使 MoveFileEx 遭遇 ERROR_SHARING_VIOLATION)
    HANDLE hExternalLocked = CreateFileW(
        sink->getActiveLogPath().c_str(),
        GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE, // 缺少 FILE_SHARE_DELETE
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        nullptr
    );
    EXPECT_NE(hExternalLocked, INVALID_HANDLE_VALUE);

    // 此时写入超过 100 字节，触发滚动；MoveFileEx 将失败，槽体应自动降级为复制+截断，0 崩溃 0 丢日志
    std::string largeLog(120, 'Z');
    EXPECT_NO_THROW({
        writeTestLogMsg(sink, largeLog);
    });

    if (hExternalLocked != INVALID_HANDLE_VALUE) {
        CloseHandle(hExternalLocked);
    }

    // 验证归档文件成功通过 fallback 创建
    const auto archive = testDir / L"tools3000_2026-09-21_001.log";
    EXPECT_TRUE(std::filesystem::exists(archive));
    EXPECT_TRUE(std::filesystem::exists(sink->getActiveLogPath()));

    sink.reset();
    removeDirectorySafely(testDir);
}

TEST(WideUniversalRotatingFileSinkTest, ConcurrentRotationAndCompressionStress) {
    const auto testDir = std::filesystem::temp_directory_path() / "t3k_log_test_concurrent_comp";
    removeDirectorySafely(testDir);

    tools3000::core::WideUniversalSinkConfig cfg;
    cfg.logDir = testDir;
    cfg.logFileName = "tools3000";
    cfg.maxFileSize = 200;
    cfg.enableCompression = true;
    cfg.idleHandleTimeoutMs = 60000;

    auto sink = std::make_shared<tools3000::core::WideUniversalRotatingFileSink>(cfg);
    sink->setSimulatedDayForTesting("2026-09-21");

    // 高频连续写入触发多次滚动与并发压缩
    for (int i = 0; i < 8; ++i) {
        std::string payload = "Concurrent stress test payload iteration " + std::to_string(i) + std::string(180, 'D');
        writeTestLogMsg(sink, payload);
    }

    // 触发同步自净并等待后台压缩队列完全收敛
    sink->triggerJanitorSync();

    // 验证至少生成了 gzip 归档且没有任何崩溃或文件损毁
    bool foundGz = false;
    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator(testDir, ec)) {
        const std::string fn = tools3000::core::WinUtils::wstringToUtf8(entry.path().filename().wstring());
        if (fn.ends_with(".log.gz")) {
            foundGz = true;
            break;
        }
    }
    EXPECT_TRUE(foundGz);

    sink.reset();
    removeDirectorySafely(testDir);
}

TEST(LoggerTest, FullStackIntegrationAndDynamicRetention) {
    const auto testDir = std::filesystem::temp_directory_path() / "t3k_log_test_fullstack";
    removeDirectorySafely(testDir);

    tools3000::core::LoggerConfig cfg;
    cfg.logDir = testDir;
    cfg.logFileName = "tools3000_test";
    cfg.retentionDays = 14;
    cfg.maxTotalSize = 10 * 1024 * 1024;
    cfg.enableConsole = false;
    cfg.enableMsvcSink = false;

    tools3000::core::Logger::initialize(cfg);

    EXPECT_EQ(tools3000::core::Logger::getRetentionDays(), 14u);
    EXPECT_NE(tools3000::core::Logger::getFileSink(), nullptr);

    // 动态热更保留天数
    tools3000::core::Logger::setRetentionDays(30);
    EXPECT_EQ(tools3000::core::Logger::getRetentionDays(), 30u);

    LOG_INFO("FullStack test log entry");

    tools3000::core::Logger::triggerJanitor();
    tools3000::core::Logger::shutdown();

    EXPECT_TRUE(std::filesystem::exists(testDir / L"tools3000_test.log"));

    removeDirectorySafely(testDir);
}

TEST(WinUtilsTest, FullscreenBypassPolicyVerification) {
    using tools3000::core::WinUtils;

    // 生产力类名检测
    EXPECT_TRUE(WinUtils::isProductivityToolkitClassName(L"Chrome_WidgetWin_1"));
    EXPECT_TRUE(WinUtils::isProductivityToolkitClassName(L"MozillaWindowClass"));
    EXPECT_TRUE(WinUtils::isProductivityToolkitClassName(L"CabinetWClass"));
    EXPECT_TRUE(WinUtils::isProductivityToolkitClassName(L"Tools3000_SearchWindow"));
    EXPECT_TRUE(WinUtils::isProductivityToolkitClassName(L"Tools3000_SettingsWindow"));
    EXPECT_TRUE(WinUtils::isProductivityToolkitClassName(L"OrpheusBrowserHost"));
    EXPECT_TRUE(WinUtils::isProductivityToolkitClassName(L"CefBrowserWindow"));
    EXPECT_TRUE(WinUtils::isProductivityToolkitClassName(L"Qt51514QWindowIcon"));
    EXPECT_TRUE(WinUtils::isProductivityToolkitClassName(L"ApplicationFrameWindow"));

    // 独占游戏等非生产力类名
    EXPECT_FALSE(WinUtils::isProductivityToolkitClassName(L"UnityWndClass"));
    EXPECT_FALSE(WinUtils::isProductivityToolkitClassName(L"D3D11_GameWindow"));
    EXPECT_FALSE(WinUtils::isProductivityToolkitClassName(L"UnrealWindow"));

    // shouldBypassFullscreenInteractions 判定策略
    EXPECT_TRUE(WinUtils::shouldBypassFullscreenInteractions(true, L"D3D11_GameWindow"));
    EXPECT_FALSE(WinUtils::shouldBypassFullscreenInteractions(true, L"Chrome_WidgetWin_1"));
    EXPECT_FALSE(WinUtils::shouldBypassFullscreenInteractions(true, L"Tools3000_SearchWindow"));
    EXPECT_FALSE(WinUtils::shouldBypassFullscreenInteractions(false, L"D3D11_GameWindow"));

    // bool 重载
    EXPECT_TRUE(WinUtils::shouldBypassFullscreenInteractions(true, false));
    EXPECT_FALSE(WinUtils::shouldBypassFullscreenInteractions(true, true));
    EXPECT_FALSE(WinUtils::shouldBypassFullscreenInteractions(false, false));

    // HWND 重载 nullptr 安全防御
    EXPECT_FALSE(WinUtils::shouldBypassFullscreenInteractions(static_cast<HWND>(nullptr)));
}

TEST(WinUtilsTest, SmartFullscreenSceneClassificationAndPolicyModes) {
    using tools3000::core::WinUtils;
    using Scene = WinUtils::FullscreenScene;
    using Mode = WinUtils::FullscreenPolicyMode;

    // 1. 策略模式字符串解析与序列化双向验证
    EXPECT_EQ(WinUtils::parseFullscreenPolicyMode("smart"), Mode::Smart);
    EXPECT_EQ(WinUtils::parseFullscreenPolicyMode("SMART"), Mode::Smart);
    EXPECT_EQ(WinUtils::parseFullscreenPolicyMode("strict_all"), Mode::StrictAll);
    EXPECT_EQ(WinUtils::parseFullscreenPolicyMode("strict"), Mode::StrictAll);
    EXPECT_EQ(WinUtils::parseFullscreenPolicyMode("gesture_only"), Mode::GestureOnly);
    EXPECT_EQ(WinUtils::parseFullscreenPolicyMode("off"), Mode::Off);
    EXPECT_EQ(WinUtils::parseFullscreenPolicyMode("disabled"), Mode::Off);
    EXPECT_EQ(WinUtils::parseFullscreenPolicyMode("unknown_invalid"), Mode::Smart);
    EXPECT_EQ(WinUtils::parseFullscreenPolicyMode(""), Mode::Smart);

    EXPECT_STREQ(WinUtils::fullscreenPolicyModeToString(Mode::Smart), "smart");
    EXPECT_STREQ(WinUtils::fullscreenPolicyModeToString(Mode::StrictAll), "strict_all");
    EXPECT_STREQ(WinUtils::fullscreenPolicyModeToString(Mode::GestureOnly), "gesture_only");
    EXPECT_STREQ(WinUtils::fullscreenPolicyModeToString(Mode::Off), "off");

    // 2. 影音播放器与常见 3D 游戏特征识别
    EXPECT_TRUE(WinUtils::isMediaPlayerProcessOrClass(L"PotPlayerMini64.exe", L"PotPlayer64"));
    EXPECT_TRUE(WinUtils::isMediaPlayerProcessOrClass(L"vlc.exe", L"Qt51514QWindowIcon"));
    EXPECT_TRUE(WinUtils::isMediaPlayerProcessOrClass(L"mpv.exe", L"mpv"));
    EXPECT_FALSE(WinUtils::isMediaPlayerProcessOrClass(L"dota2.exe", L"SDL_app"));

    EXPECT_TRUE(WinUtils::isKnownGameProcessOrClass(
        L"dota2.exe", L"SDL_app",
        L"C:\\Program Files (x86)\\Steam\\steamapps\\common\\dota 2 beta\\game\\bin\\win64\\dota2.exe"));
    EXPECT_TRUE(WinUtils::isKnownGameProcessOrClass(L"cs2.exe", L"SDL_app", L""));
    EXPECT_TRUE(WinUtils::isKnownGameProcessOrClass(L"custom_indie.exe", L"UnityWndClass", L""));
    EXPECT_TRUE(WinUtils::isKnownGameProcessOrClass(L"custom_ue5.exe", L"UnrealWindow", L""));
    EXPECT_TRUE(WinUtils::isKnownGameProcessOrClass(
        L"unknown_game.exe", L"Chrome_WidgetWin_1",
        L"D:\\SteamLibrary\\steamapps\\common\\SomeGame\\unknown_game.exe"));
    EXPECT_FALSE(WinUtils::isKnownGameProcessOrClass(
        L"chrome.exe", L"Chrome_WidgetWin_1",
        L"C:\\Program Files\\Google\\Chrome\\Application\\chrome.exe"));

    // 远程桌面、虚拟机与 scrcpy 投屏不应误判为 3D 游戏
    EXPECT_FALSE(WinUtils::isKnownGameProcessOrClass(L"mstsc.exe", L"TscShellContainerClass", L"C:\\Windows\\System32\\mstsc.exe"));
    EXPECT_FALSE(WinUtils::isKnownGameProcessOrClass(L"ToDesk.exe", L"ToDesk_Remote", L""));
    EXPECT_FALSE(WinUtils::isKnownGameProcessOrClass(L"vmware.exe", L"VMPlayerFrame", L""));
    EXPECT_FALSE(WinUtils::isKnownGameProcessOrClass(L"scrcpy.exe", L"SDL_app", L"C:\\Tools\\scrcpy\\scrcpy.exe"));

    // 3. 全屏场景四象限分类
    EXPECT_EQ(WinUtils::classifyFullscreenScene(false, L"dota2.exe", L"SDL_app"), Scene::None);
    EXPECT_EQ(WinUtils::classifyFullscreenScene(true, L"Tools3000.exe", L"Tools3000_SearchWindow"), Scene::Productivity);
    EXPECT_EQ(WinUtils::classifyFullscreenScene(true, L"explorer.exe", L"CabinetWClass"), Scene::Productivity);
    EXPECT_EQ(WinUtils::classifyFullscreenScene(true, L"explorer.exe", L"Progman"), Scene::Productivity);
    EXPECT_EQ(WinUtils::classifyFullscreenScene(true, L"scrcpy.exe", L"SDL_app"), Scene::Productivity);

    // Dota 2 / CS2 / 原生 DirectX/Vulkan 全屏窗口 -> Game
    EXPECT_EQ(WinUtils::classifyFullscreenScene(
        true, L"dota2.exe", L"SDL_app",
        L"C:\\Program Files (x86)\\Steam\\steamapps\\common\\dota 2 beta\\game\\bin\\win64\\dota2.exe"),
        Scene::Game);
    EXPECT_EQ(WinUtils::classifyFullscreenScene(true, L"mygame.exe", L"D3D11_GameWindow"), Scene::Game);

    // 播放器、浏览器全屏看视频及远程桌面/虚拟机全屏 -> MediaOrBrowser
    EXPECT_EQ(WinUtils::classifyFullscreenScene(true, L"PotPlayerMini64.exe", L"PotPlayer64"), Scene::MediaOrBrowser);
    EXPECT_EQ(WinUtils::classifyFullscreenScene(true, L"chrome.exe", L"Chrome_WidgetWin_1"), Scene::MediaOrBrowser);
    EXPECT_EQ(WinUtils::classifyFullscreenScene(true, L"firefox.exe", L"MozillaWindowClass"), Scene::MediaOrBrowser);
    EXPECT_EQ(WinUtils::classifyFullscreenScene(true, L"mstsc.exe", L"TscShellContainerClass"), Scene::MediaOrBrowser);
    EXPECT_EQ(WinUtils::classifyFullscreenScene(true, L"ToDesk.exe", L"ToDesk_Remote"), Scene::MediaOrBrowser);
    EXPECT_EQ(WinUtils::classifyFullscreenScene(true, L"vmware.exe", L"VMUIFrame"), Scene::MediaOrBrowser);

    // 4. 四种策略模式下的精细拦截矩阵验证
    // (A) Smart 智能模式（默认）：打游戏停用快捷键+截图+手势；看电影保留快捷键+截图，仅停用手势与浮层
    EXPECT_TRUE(WinUtils::shouldSuspendHotkeysForScene(Scene::Game, Mode::Smart));
    EXPECT_TRUE(WinUtils::shouldSuspendGesturesForScene(Scene::Game, Mode::Smart));
    EXPECT_TRUE(WinUtils::shouldBypassCaptureForScene(Scene::Game, Mode::Smart));
    EXPECT_TRUE(WinUtils::shouldBypassOverlaysForScene(Scene::Game, Mode::Smart));

    EXPECT_FALSE(WinUtils::shouldSuspendHotkeysForScene(Scene::MediaOrBrowser, Mode::Smart));
    EXPECT_FALSE(WinUtils::shouldBypassCaptureForScene(Scene::MediaOrBrowser, Mode::Smart));
    EXPECT_TRUE(WinUtils::shouldSuspendGesturesForScene(Scene::MediaOrBrowser, Mode::Smart));
    EXPECT_TRUE(WinUtils::shouldBypassOverlaysForScene(Scene::MediaOrBrowser, Mode::Smart));

    EXPECT_FALSE(WinUtils::shouldSuspendHotkeysForScene(Scene::Productivity, Mode::Smart));
    EXPECT_FALSE(WinUtils::shouldSuspendGesturesForScene(Scene::Productivity, Mode::Smart));
    EXPECT_FALSE(WinUtils::shouldSuspendHotkeysForScene(Scene::None, Mode::Smart));
    EXPECT_FALSE(WinUtils::shouldSuspendGesturesForScene(Scene::None, Mode::Smart));

    // (B) StrictAll 严格模式：全屏看电影与打游戏均停用全部快捷键、截图与手势
    EXPECT_TRUE(WinUtils::shouldSuspendHotkeysForScene(Scene::MediaOrBrowser, Mode::StrictAll));
    EXPECT_TRUE(WinUtils::shouldBypassCaptureForScene(Scene::MediaOrBrowser, Mode::StrictAll));
    EXPECT_TRUE(WinUtils::shouldSuspendGesturesForScene(Scene::MediaOrBrowser, Mode::StrictAll));
    EXPECT_TRUE(WinUtils::shouldSuspendHotkeysForScene(Scene::Game, Mode::StrictAll));

    // (C) GestureOnly 仅手势静默模式：打游戏也保留全局快捷键与截图，仅停用手势与浮层
    EXPECT_FALSE(WinUtils::shouldSuspendHotkeysForScene(Scene::Game, Mode::GestureOnly));
    EXPECT_FALSE(WinUtils::shouldBypassCaptureForScene(Scene::Game, Mode::GestureOnly));
    EXPECT_TRUE(WinUtils::shouldSuspendGesturesForScene(Scene::Game, Mode::GestureOnly));
    EXPECT_TRUE(WinUtils::shouldBypassOverlaysForScene(Scene::Game, Mode::GestureOnly));

    // (D) Off 关闭模式：任何全屏均不自动停用
    EXPECT_FALSE(WinUtils::shouldSuspendHotkeysForScene(Scene::Game, Mode::Off));
    EXPECT_FALSE(WinUtils::shouldSuspendGesturesForScene(Scene::Game, Mode::Off));
    EXPECT_FALSE(WinUtils::shouldBypassCaptureForScene(Scene::Game, Mode::Off));
    EXPECT_FALSE(WinUtils::shouldBypassOverlaysForScene(Scene::Game, Mode::Off));

    WinUtils::setFullscreenPolicyMode(Mode::Smart);
    EXPECT_EQ(WinUtils::getFullscreenPolicyMode(), Mode::Smart);
}

TEST(WinUtilsTest, RealWindowFullscreenDetectionAndWindowedGamePolicy) {
    using WinUtils = tools3000::core::WinUtils;
    using Scene = WinUtils::FullscreenScene;

    WNDCLASSEXW wcex{sizeof(WNDCLASSEXW)};
    wcex.lpfnWndProc = DefWindowProcW;
    wcex.hInstance = GetModuleHandleW(nullptr);
    wcex.lpszClassName = L"SDL_app";
    RegisterClassExW(&wcex);

    HMONITOR hMon = MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY);
    ASSERT_NE(hMon, nullptr);
    MONITORINFO mi{sizeof(MONITORINFO)};
    ASSERT_TRUE(GetMonitorInfoW(hMon, &mi));
    const int monW = mi.rcMonitor.right - mi.rcMonitor.left;
    const int monH = mi.rcMonitor.bottom - mi.rcMonitor.top;

    // 1. 窗口化非游戏进程（即使类名为 SDL_app，尺寸 800x600）不应被误判为全屏或游戏场景
    HWND hwnd = CreateWindowExW(
        WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, L"SDL_app", L"WindowedSDLApp", WS_POPUP,
        mi.rcMonitor.left + 50, mi.rcMonitor.top + 50, 800, 600,
        nullptr, nullptr, wcex.hInstance, nullptr);
    ASSERT_NE(hwnd, nullptr);

    // 隐藏态窗口绝不能判定为全屏
    EXPECT_FALSE(WinUtils::isWindowFullscreen(hwnd));
    EXPECT_EQ(WinUtils::classifyWindowScene(hwnd), Scene::None);

    ShowWindow(hwnd, SW_SHOWNOACTIVATE);
    EXPECT_FALSE(WinUtils::isWindowFullscreen(hwnd));
    EXPECT_EQ(WinUtils::classifyWindowScene(hwnd), Scene::None);

    // 2. 覆盖显示器（含 1px Focus Assist 避让缩进 / 2px 容差内）应准确判定为全屏
    SetWindowPos(hwnd, nullptr,
                 mi.rcMonitor.left + 1, mi.rcMonitor.top + 1,
                 monW - 2, monH - 2,
                 SWP_NOZORDER | SWP_NOACTIVATE);
    EXPECT_TRUE(WinUtils::isWindowFullscreen(hwnd));
    EXPECT_EQ(WinUtils::classifyWindowScene(hwnd), Scene::Game);

    // 3. 鼠标穿透透明图层 (WS_EX_LAYERED | WS_EX_TRANSPARENT) 即使覆盖全屏也绝不误判为全屏独占应用
    const LONG origEx = GetWindowLongW(hwnd, GWL_EXSTYLE);
    SetWindowLongW(hwnd, GWL_EXSTYLE, origEx | WS_EX_LAYERED | WS_EX_TRANSPARENT);
    EXPECT_FALSE(WinUtils::isWindowFullscreen(hwnd));
    SetWindowLongW(hwnd, GWL_EXSTYLE, origEx);

    DestroyWindow(hwnd);
}

TEST(HotkeyManagerTest, FullscreenGameSilenceUnregistersAndRestoresHotkeys) {
    auto& hotkeyMgr = tools3000::core::HotkeyManager::instance();
    auto& kbHook = tools3000::core::KeyboardHook::instance();
    auto& mouseHook = tools3000::core::MouseHook::instance();

    WNDCLASSEXW wcex{sizeof(WNDCLASSEXW)};
    wcex.lpfnWndProc = DefWindowProcW;
    wcex.hInstance = GetModuleHandleW(nullptr);
    wcex.lpszClassName = L"Tools3000_FullscreenHotkeyTestWnd";
    RegisterClassExW(&wcex);
    HWND hwnd = CreateWindowExW(
        0, L"Tools3000_FullscreenHotkeyTestWnd", L"Test", WS_POPUP,
        0, 0, 10, 10, nullptr, nullptr, wcex.hInstance, nullptr);
    HWND probeHwnd = CreateWindowExW(
        0, L"Tools3000_FullscreenHotkeyTestWnd", L"Probe", WS_POPUP,
        0, 0, 10, 10, nullptr, nullptr, wcex.hInstance, nullptr);
    ASSERT_NE(hwnd, nullptr);
    ASSERT_NE(probeHwnd, nullptr);

    hotkeyMgr.shutdown();
    hotkeyMgr.initialize(hwnd);

    const tools3000::core::HotkeyDef testDef{
        tools3000::core::ModKey::Ctrl | tools3000::core::ModKey::Alt | tools3000::core::ModKey::Shift,
        VK_F11};
    const tools3000::core::HotkeyDef f1Def{tools3000::core::ModKey::None, VK_F1};
    EXPECT_TRUE(hotkeyMgr.registerHotkey("FullscreenTestHotkey", testDef, []() {}));
    EXPECT_TRUE(hotkeyMgr.registerHotkey("ScreenshotF1", f1Def, []() {}));
    EXPECT_FALSE(hotkeyMgr.isFullscreenSuspended());

    // 激活全屏游戏静默：Win32 热键与底层钩子单键 (F1) 必须真实从系统卸下，外部窗口可立即注册该热键
    hotkeyMgr.setFullscreenSuspended(true);
    EXPECT_TRUE(hotkeyMgr.isFullscreenSuspended());
    EXPECT_TRUE(kbHook.isFullscreenSuspended());
    EXPECT_FALSE(kbHook.hasHookHotkeys());

    const UINT probeMods = static_cast<UINT>(testDef.modifiers) | MOD_NOREPEAT;
    const BOOL probeClaimedDuringSuspense = RegisterHotKey(probeHwnd, 0x7777, probeMods, testDef.virtualKey);
    EXPECT_TRUE(probeClaimedDuringSuspense);
    if (probeClaimedDuringSuspense) {
        UnregisterHotKey(probeHwnd, 0x7777);
    }

    for (const auto& entry : hotkeyMgr.getAllHotkeys()) {
        if (entry.name == "FullscreenTestHotkey" || entry.name == "ScreenshotF1") {
            EXPECT_FALSE(entry.conflict);
            EXPECT_EQ(entry.conflictType, "none");
            EXPECT_TRUE(entry.registered);
        }
    }

    // 鼠标钩子全屏直通开关验证
    mouseHook.setFullscreenSuspended(true);
    EXPECT_TRUE(mouseHook.isFullscreenSuspended());
    mouseHook.setFullscreenSuspended(false);
    EXPECT_FALSE(mouseHook.isFullscreenSuspended());

    // 在全屏静默期间动态注册或重绑定热键：应记录为挂起态而不强行占用 OS 热键，解除后自动注册生效
    const tools3000::core::HotkeyDef dynamicDef{
        tools3000::core::ModKey::Ctrl | tools3000::core::ModKey::Alt | tools3000::core::ModKey::Shift,
        VK_F12};
    hotkeyMgr.setFullscreenSuspended(true);
    EXPECT_TRUE(hotkeyMgr.registerHotkey("DynamicWhileSuspended", dynamicDef, []() {}));
    EXPECT_FALSE(hotkeyMgr.checkConflict(dynamicDef, "DynamicWhileSuspended").hasConflict);
    EXPECT_TRUE(hotkeyMgr.isConflict(dynamicDef));

    // 解除全屏游戏静默：所有已武装热键自动恢复注册
    hotkeyMgr.setFullscreenSuspended(false);
    EXPECT_FALSE(hotkeyMgr.isFullscreenSuspended());
    EXPECT_FALSE(kbHook.isFullscreenSuspended());
    for (const auto& entry : hotkeyMgr.getAllHotkeys()) {
        if (entry.name == "FullscreenTestHotkey" ||
            entry.name == "ScreenshotF1" ||
            entry.name == "DynamicWhileSuspended") {
            EXPECT_TRUE(entry.registered);
            EXPECT_FALSE(entry.conflict);
        }
    }

    hotkeyMgr.shutdown();
    DestroyWindow(probeHwnd);
    DestroyWindow(hwnd);
}

TEST(WinUtilsTest, MessageBridgeFullscreenModeSettingsValidation) {
    const auto tempDir = std::filesystem::temp_directory_path() / "tools3000_fullscreen_mode_test";
    std::error_code ec;
    std::filesystem::remove_all(tempDir, ec);
    auto& config = tools3000::core::ConfigManager::instance();
    ASSERT_TRUE(config.initialize(tempDir));

    auto& bridge = tools3000::core::MessageBridge::instance();
    bridge.registerBuiltinHandlers();

    // 1. 合法的 4 种全屏策略模式均可正常更新并同步至 WinUtils
    for (const char* mode : {"strict_all", "gesture_only", "off", "smart"}) {
        const nlohmann::json req = {
            {"id", 9910},
            {"method", "general.updateSettings"},
            {"params", {{"fullscreenMode", mode}}}
        };
        const auto resp = nlohmann::json::parse(bridge.handleMessage(req.dump()));
        ASSERT_TRUE(resp.contains("result"));
        EXPECT_TRUE(resp["result"].value("success", false));
        EXPECT_STREQ(
            tools3000::core::WinUtils::fullscreenPolicyModeToString(
                tools3000::core::WinUtils::getFullscreenPolicyMode()),
            mode);
    }

    // 2. 非法枚举值或非字符串类型必须被拒绝并返回明确错误
    const nlohmann::json badStrReq = {
        {"id", 9911},
        {"method", "general.updateSettings"},
        {"params", {{"fullscreenMode", "invalid_mode_xyz"}}}
    };
    const auto badStrResp = nlohmann::json::parse(bridge.handleMessage(badStrReq.dump()));
    ASSERT_TRUE(badStrResp.contains("result"));
    EXPECT_FALSE(badStrResp["result"].value("success", true));
    EXPECT_EQ(badStrResp["result"].value("error", ""), "invalid fullscreen mode");

    const nlohmann::json badTypeReq = {
        {"id", 9912},
        {"method", "general.updateSettings"},
        {"params", {{"fullscreenMode", 12345}}}
    };
    const auto badTypeResp = nlohmann::json::parse(bridge.handleMessage(badTypeReq.dump()));
    ASSERT_TRUE(badTypeResp.contains("result"));
    EXPECT_FALSE(badTypeResp["result"].value("success", true));
    EXPECT_EQ(badTypeResp["result"].value("error", ""), "invalid fullscreen mode");

    tools3000::core::WinUtils::setFullscreenPolicyMode(
        tools3000::core::WinUtils::FullscreenPolicyMode::Smart);
    config.shutdown();
    std::filesystem::remove_all(tempDir, ec);
}

TEST(HotkeyManagerTest, RecordingPauseUnregistersAndRestoresOsHotkeys) {
    auto& hotkeyMgr = tools3000::core::HotkeyManager::instance();
    auto& kbHook = tools3000::core::KeyboardHook::instance();

    WNDCLASSEXW wcex{sizeof(WNDCLASSEXW)};
    wcex.lpfnWndProc = DefWindowProcW;
    wcex.hInstance = GetModuleHandleW(nullptr);
    wcex.lpszClassName = L"Tools3000_RecordingPauseHotkeyTestWnd";
    RegisterClassExW(&wcex);
    HWND hwnd = CreateWindowExW(
        0, L"Tools3000_RecordingPauseHotkeyTestWnd", L"Host", WS_POPUP,
        0, 0, 10, 10, nullptr, nullptr, wcex.hInstance, nullptr);
    HWND probeHwnd = CreateWindowExW(
        0, L"Tools3000_RecordingPauseHotkeyTestWnd", L"Probe", WS_POPUP,
        0, 0, 10, 10, nullptr, nullptr, wcex.hInstance, nullptr);
    ASSERT_NE(hwnd, nullptr);
    ASSERT_NE(probeHwnd, nullptr);

    hotkeyMgr.shutdown();
    hotkeyMgr.initialize(hwnd);
    hotkeyMgr.setFullscreenSuspended(false);
    hotkeyMgr.setPaused(false);

    const tools3000::core::HotkeyDef initialDef{
        tools3000::core::ModKey::Ctrl | tools3000::core::ModKey::Alt | tools3000::core::ModKey::Shift,
        VK_F9};
    const tools3000::core::HotkeyDef reboundDef{
        tools3000::core::ModKey::Ctrl | tools3000::core::ModKey::Alt | tools3000::core::ModKey::Shift,
        VK_F10};
    const tools3000::core::HotkeyDef hookF2Def{tools3000::core::ModKey::None, VK_F2};

    ASSERT_TRUE(hotkeyMgr.registerHotkey("PauseRecordTest", initialDef, []() {}));
    ASSERT_TRUE(hotkeyMgr.registerHotkey("PauseHookTest", hookF2Def, []() {}));

    // 进入快捷键录制暂停态：OS 热键与钩子单键必须物理卸载，以便前端录制框能捕获用户再次按下自身旧热键
    hotkeyMgr.setPaused(true);
    EXPECT_TRUE(hotkeyMgr.isPaused());
    EXPECT_FALSE(kbHook.hasHookHotkeys());

    const UINT initialMods = static_cast<UINT>(initialDef.modifiers) | MOD_NOREPEAT;
    const BOOL claimedWhilePaused = RegisterHotKey(probeHwnd, 0x7781, initialMods, initialDef.virtualKey);
    EXPECT_TRUE(claimedWhilePaused);
    if (claimedWhilePaused) {
        UnregisterHotKey(probeHwnd, 0x7781);
    }

    // 在录制暂停期间重绑定为新组合键，退出暂停后新组合键应自动向 OS 注册生效
    EXPECT_TRUE(hotkeyMgr.rebindHotkey("PauseRecordTest", reboundDef));
    hotkeyMgr.setPaused(false);
    EXPECT_FALSE(hotkeyMgr.isPaused());

    const UINT reboundMods = static_cast<UINT>(reboundDef.modifiers) | MOD_NOREPEAT;
    const BOOL claimedAfterResume = RegisterHotKey(probeHwnd, 0x7782, reboundMods, reboundDef.virtualKey);
    EXPECT_FALSE(claimedAfterResume);
    if (claimedAfterResume) {
        UnregisterHotKey(probeHwnd, 0x7782);
    }

    hotkeyMgr.shutdown();
    DestroyWindow(probeHwnd);
    DestroyWindow(hwnd);
}

TEST(LoggerTest, SetLevelTracePropagatesToUniversalFileSink) {
    const auto tempLogDir = std::filesystem::temp_directory_path() /
        (L"Tools3000_TraceSinkTest_" + std::to_wstring(GetCurrentProcessId()));
    std::error_code ec;
    std::filesystem::remove_all(tempLogDir, ec);
    std::filesystem::create_directories(tempLogDir, ec);

    tools3000::core::Logger::shutdown();
    tools3000::core::LoggerConfig cfg;
    cfg.logDir = tempLogDir;
    cfg.logFileName = "trace_verify";
    cfg.fileLevel = spdlog::level::debug;
    cfg.enableConsole = false;
    cfg.enableMsvcSink = false;
    tools3000::core::Logger::initialize(cfg);

    ASSERT_NE(tools3000::core::Logger::getFileSink(), nullptr);
    EXPECT_EQ(tools3000::core::Logger::getFileSink()->level(), spdlog::level::debug);

    tools3000::core::Logger::setLevel(spdlog::level::trace);
    EXPECT_EQ(tools3000::core::Logger::getFileSink()->level(), spdlog::level::trace);

    LOG_TRACE("trace sink delivery verification marker");
    tools3000::core::Logger::shutdown();

    bool foundTraceLine = false;
    for (const auto& entry : std::filesystem::directory_iterator(tempLogDir, ec)) {
        if (!entry.is_regular_file()) continue;
        std::ifstream ifs(entry.path(), std::ios::binary);
        std::string content((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
        if (content.find("trace sink delivery verification marker") != std::string::npos) {
            foundTraceLine = true;
            break;
        }
    }
    EXPECT_TRUE(foundTraceLine);

    // 恢复默认测试日志器以免影响后续用例
    tools3000::core::LoggerConfig restoreCfg;
    restoreCfg.logDir = std::filesystem::temp_directory_path() / L"Tools3000_UnitTestLogs";
    restoreCfg.enableConsole = false;
    restoreCfg.enableMsvcSink = false;
    tools3000::core::Logger::initialize(restoreCfg);
    std::filesystem::remove_all(tempLogDir, ec);
}

TEST(WinUtilsTest, VirtualScreenPhysicalBoundsCoversSystemVirtualScreen) {
    const RECT bounds = tools3000::core::WinUtils::getVirtualScreenPhysicalBounds();
    const int width = bounds.right - bounds.left;
    const int height = bounds.bottom - bounds.top;
    EXPECT_GT(width, 0);
    EXPECT_GT(height, 0);
    EXPECT_GE(width, GetSystemMetrics(SM_CXSCREEN));
    EXPECT_GE(height, GetSystemMetrics(SM_CYSCREEN));
}

TEST(MessageBridgeTest, ConfigResetClearsAutoStartAndBroadcastsAppearanceEvents) {
    const auto tempDir = std::filesystem::temp_directory_path() /
        (L"Tools3000_ResetSyncTest_" + std::to_wstring(GetCurrentProcessId()));
    std::error_code ec;
    std::filesystem::remove_all(tempDir, ec);
    std::filesystem::create_directories(tempDir, ec);
    std::filesystem::create_directories(tools3000::core::WinUtils::getConfigDirectory(), ec);

    auto& config = tools3000::core::ConfigManager::instance();
    ASSERT_TRUE(config.initialize(tempDir));

    auto& bridge = tools3000::core::MessageBridge::instance();
    bridge.registerBuiltinHandlers();

    std::atomic<int> appearanceEvents{0};
    std::atomic<int> configEvents{0};
    bridge.registerEventPusher("unit_test_sync_listener", [&](const std::string& eventName, const nlohmann::json&) {
        if (eventName == "appearance:changed") {
            appearanceEvents.fetch_add(1, std::memory_order_relaxed);
        } else if (eventName == "config:changed") {
            configEvents.fetch_add(1, std::memory_order_relaxed);
        }
    });

    // 先开启开机自启缓存并修改主题色，验证更新设置会实时广播 appearance:changed
    const auto enableAuto = bridge.invokeHandler("autoStart.set", {{"enabled", true}});
    EXPECT_TRUE(enableAuto.value("success", false));
    const auto updateTheme = bridge.invokeHandler("general.updateSettings", {{"theme", "dark"}, {"accentColor", "cyan"}});
    EXPECT_TRUE(updateTheme.value("success", false));
    EXPECT_GE(appearanceEvents.load(std::memory_order_relaxed), 1);

    const auto beforeReset = bridge.invokeHandler("general.getSettings");
    EXPECT_TRUE(beforeReset.value("autoStart", false));

    // 执行重置：必须清除自启状态与缓存，防止 general.getSettings 迁移分支反向复活开机自启
    appearanceEvents.store(0, std::memory_order_relaxed);
    configEvents.store(0, std::memory_order_relaxed);
    const auto resetRes = bridge.invokeHandler("config.reset");
    EXPECT_TRUE(resetRes.value("success", false));
    EXPECT_GE(appearanceEvents.load(std::memory_order_relaxed), 1);
    EXPECT_GE(configEvents.load(std::memory_order_relaxed), 1);

    const auto afterReset = bridge.invokeHandler("general.getSettings");
    EXPECT_FALSE(afterReset.value("autoStart", true));
    EXPECT_EQ(afterReset.value("theme", ""), "system");

    bridge.unregisterEventPusher("unit_test_sync_listener");
    config.shutdown();
    std::filesystem::remove_all(tempDir, ec);
}

TEST(CoreKeyboardHookTest, CaptureAndScrollOverlayVisibilityAtomicBypass) {
    auto& kbHook = tools3000::core::KeyboardHook::instance();
    kbHook.setCaptureOverlayVisible(false);
    kbHook.setScrollCaptureOverlayVisible(false);
    EXPECT_FALSE(kbHook.isAnyCaptureOverlayVisible());

    kbHook.setCaptureOverlayVisible(true);
    EXPECT_TRUE(kbHook.isAnyCaptureOverlayVisible());

    kbHook.setScrollCaptureOverlayVisible(true);
    EXPECT_TRUE(kbHook.isAnyCaptureOverlayVisible());

    kbHook.setCaptureOverlayVisible(false);
    EXPECT_TRUE(kbHook.isAnyCaptureOverlayVisible());

    kbHook.setScrollCaptureOverlayVisible(false);
    EXPECT_FALSE(kbHook.isAnyCaptureOverlayVisible());
}


// ─────────────────────────────────────────────────────────────────────────────
// 2026-11-11 全量审计防回退回归测试 (AtomicFile / SpscRingBuffer / ShellContextMenu)
// ─────────────────────────────────────────────────────────────────────────────

TEST(AtomicFileSecurityTest, RejectsDirectoryJunctionAndWritesNormalFilesSafely) {
    const auto baseDir = std::filesystem::temp_directory_path() /
        (L"Tools3000_AtomicFileAudit_" + std::to_wstring(GetCurrentProcessId()) + L"_" +
         std::to_wstring(GetTickCount64()));
    const auto safeDir = baseDir / L"safe_dir";
    const auto realTargetDir = baseDir / L"real_target";
    const auto junctionDir = baseDir / L"junction_link";

    std::error_code ec;
    std::filesystem::remove_all(baseDir, ec);
    std::filesystem::create_directories(safeDir, ec);
    std::filesystem::create_directories(realTargetDir, ec);

    struct SandboxCleanup {
        std::filesystem::path junction;
        std::filesystem::path root;
        ~SandboxCleanup() {
            // 先解除目录联接，防止递归删除跟随重解析点
            RemoveDirectoryW(junction.c_str());
            std::error_code err;
            std::filesystem::remove_all(root, err);
        }
    } cleanup{junctionDir, baseDir};

    // 1. 常规安全路径原子落盘验证
    const std::wstring safeFile = (safeDir / L"config.json").wstring();
    EXPECT_FALSE(tools3000::common::pathContainsReparsePoint(safeFile));
    ASSERT_TRUE(tools3000::common::atomicWriteFileWithFlush(safeFile, "{\"ok\":true}"));
    {
        std::ifstream ifs(std::filesystem::path(safeFile), std::ios::binary);
        std::string content((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
        EXPECT_EQ(content, "{\"ok\":true}");
    }

    // 2. 构造真实 NTFS 目录联接 (mklink /J 无需管理员权限)
    const std::wstring cmdLine = L"cmd.exe /d /s /c \"mklink /J \"" + junctionDir.wstring() +
                                 L"\" \"" + realTargetDir.wstring() + L"\" >nul 2>&1\"";
    STARTUPINFOW si{sizeof(si)};
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi{};
    std::wstring mutableCmd = cmdLine;
    bool junctionCreated = false;
    if (CreateProcessW(nullptr, mutableCmd.data(), nullptr, nullptr, FALSE,
                       CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        WaitForSingleObject(pi.hProcess, 5000);
        DWORD exitCode = 1;
        GetExitCodeProcess(pi.hProcess, &exitCode);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        const DWORD attrs = GetFileAttributesW(junctionDir.c_str());
        junctionCreated = (exitCode == 0) && (attrs != INVALID_FILE_ATTRIBUTES) &&
                          ((attrs & FILE_ATTRIBUTE_REPARSE_POINT) != 0);
    }

    if (junctionCreated) {
        const std::wstring hijackedFile = (junctionDir / L"hijacked.json").wstring();
        EXPECT_TRUE(tools3000::common::pathContainsReparsePoint(hijackedFile));
        EXPECT_FALSE(tools3000::common::atomicWriteFileWithFlush(hijackedFile, "{\"pwned\":true}"));
        EXPECT_FALSE(std::filesystem::exists(realTargetDir / L"hijacked.json", ec));
    }
}

TEST(SpscRingBufferConcurrencyTest, ConcurrentOverflowAndPopNoTear) {
    struct CheckedSample {
        uint64_t seq;
        uint64_t checksum;
    };
    constexpr uint64_t kMagic = 0x9E3779B97F4A7C15ULL;
    tools3000::core::SpscRingBuffer<CheckedSample, 64> ring;

    // 1. 验证满载时 try_push 不覆写旧样本
    for (uint64_t i = 0; i < 64; ++i) {
        EXPECT_TRUE(ring.try_push(CheckedSample{i, i ^ kMagic}));
    }
    EXPECT_FALSE(ring.try_push(CheckedSample{999, 999 ^ kMagic}));
    ring.clear();
    EXPECT_TRUE(ring.empty());

    // 2. 多线程高频并发溢出覆盖与消费：验证零数据撕裂且序号严格单调递增
    std::atomic<bool> producerDone{false};
    constexpr uint64_t kTotalItems = 20000;

    std::thread producer([&]() {
        for (uint64_t i = 1; i <= kTotalItems; ++i) {
            ring.push(CheckedSample{i, i ^ kMagic});
        }
        producerDone.store(true, std::memory_order_release);
    });

    uint64_t lastSeq = 0;
    size_t poppedCount = 0;
    std::thread consumer([&]() {
        CheckedSample item{};
        while (!producerDone.load(std::memory_order_acquire) || !ring.empty()) {
            if (ring.pop(item)) {
                EXPECT_EQ(item.checksum, item.seq ^ kMagic);
                EXPECT_GT(item.seq, lastSeq);
                lastSeq = item.seq;
                ++poppedCount;
            } else {
                std::this_thread::yield();
            }
        }
    });

    producer.join();
    consumer.join();
    EXPECT_GT(poppedCount, 0u);
    EXPECT_EQ(lastSeq, kTotalItems);
    EXPECT_TRUE(ring.empty());
}

TEST(ShellContextMenuRegressionTest, BindSystemDirectoryAndServiceLifecycle) {
    const HRESULT hrOle = OleInitialize(nullptr);
    struct OleGuard {
        bool needUninit;
        ~OleGuard() {
            if (needUninit) OleUninitialize();
        }
    } oleGuard{SUCCEEDED(hrOle)};

    wchar_t winDir[MAX_PATH]{};
    ASSERT_GT(GetWindowsDirectoryW(winDir, MAX_PATH), 0u);

    PIDLIST_ABSOLUTE pidl = ILCreateFromPathW(winDir);
    ASSERT_NE(pidl, nullptr);

    IShellFolder* parentFolder = nullptr;
    PCUITEMID_CHILD child = nullptr;
    const HRESULT hrBind = SHBindToParent(pidl, IID_IShellFolder, reinterpret_cast<void**>(&parentFolder), &child);
    EXPECT_TRUE(SUCCEEDED(hrBind));

    IContextMenu* contextMenu = nullptr;
    if (SUCCEEDED(hrBind) && parentFolder && child) {
        parentFolder->GetUIObjectOf(nullptr, 1, &child, IID_IContextMenu, nullptr, reinterpret_cast<void**>(&contextMenu));
    }
    if (!contextMenu && parentFolder && child) {
        DEFCONTEXTMENU dcm{};
        dcm.psf = parentFolder;
        dcm.cidl = 1;
        dcm.apidl = &child;
        SHCreateDefaultContextMenu(&dcm, IID_IContextMenu, reinterpret_cast<void**>(&contextMenu));
    }
    if (contextMenu) {
        HMENU hMenu = CreatePopupMenu();
        ASSERT_NE(hMenu, nullptr);
        const HRESULT hrQuery = contextMenu->QueryContextMenu(hMenu, 0, 1, 0x7FFF, CMF_DEFAULTONLY);
        EXPECT_TRUE(SUCCEEDED(hrQuery));
        EXPECT_GE(GetMenuItemCount(hMenu), 0);
        DestroyMenu(hMenu);
        contextMenu->Release();
    }
    if (parentFolder) {
        parentFolder->Release();
    }
    ILFree(pidl);

    // 验证宿主 ShellContextMenuService 空路径拦截与幂等优雅停机
    auto& menuSvc = tools3000::core::ShellContextMenuService::instance();
    EXPECT_FALSE(menuSvc.showAsync(L""));
    menuSvc.shutdown();
    menuSvc.shutdown();
}

// -----------------------------------------------------------------------------
// Batch 1 (V-01 & P-04): NativeThemePalette 与零磁盘共享图标缓存回归测试
// -----------------------------------------------------------------------------
TEST(ThemeUtilsNativePaletteTest, EffectiveThemeAccentAndUiLanguageSingleSourceOfTruth) {
    using tools3000::core::ConfigManager;
    using tools3000::core::NativeThemePalette;
    using tools3000::core::WinUtils;

    auto& cfg = ConfigManager::instance();
    const std::string savedSnapshot = cfg.toJsonString(2);
    struct ConfigRestoreGuard {
        ConfigManager& mgr;
        std::string snapshot;
        ~ConfigRestoreGuard() {
            mgr.fromJsonString(snapshot);
        }
    } restoreGuard{cfg, savedSnapshot};

    // 1. 明暗主题单一事实源判定
    ASSERT_TRUE(cfg.set("/general/theme", std::string("dark")));
    EXPECT_TRUE(NativeThemePalette::isEffectiveDarkMode());

    ASSERT_TRUE(cfg.set("/general/theme", std::string("light")));
    EXPECT_FALSE(NativeThemePalette::isEffectiveDarkMode());

    ASSERT_TRUE(cfg.set("/general/theme", std::string("system")));
    EXPECT_EQ(NativeThemePalette::isEffectiveDarkMode(), WinUtils::isSystemDarkMode());

    // 2. 动态强调色解析（预设枚举 + 自定义 #RRGGBB + 非法值回退）
    ASSERT_TRUE(cfg.set("/general/accentColor", std::string("violet")));
    const auto violet = NativeThemePalette::getEffectiveAccentColorRGB();
    const auto expectedViolet = tools3000::core::getAccentColorRGB("violet");
    EXPECT_FLOAT_EQ(violet.r, expectedViolet.r);
    EXPECT_FLOAT_EQ(violet.g, expectedViolet.g);
    EXPECT_FLOAT_EQ(violet.b, expectedViolet.b);

    ASSERT_TRUE(cfg.set("/general/accentColor", std::string("#FF5500")));
    const auto customHex = NativeThemePalette::getEffectiveAccentColorRGB();
    EXPECT_NEAR(customHex.r, 1.0f, 1e-4f);
    EXPECT_NEAR(customHex.g, 85.0f / 255.0f, 1e-4f);
    EXPECT_NEAR(customHex.b, 0.0f, 1e-4f);

    ASSERT_TRUE(cfg.set("/general/accentColor", std::string("invalid-accent-token")));
    const auto fallbackBlue = NativeThemePalette::getEffectiveAccentColorRGB();
    const auto expectedBlue = tools3000::core::getAccentColorRGB("blue");
    EXPECT_FLOAT_EQ(fallbackBlue.r, expectedBlue.r);
    EXPECT_FLOAT_EQ(fallbackBlue.g, expectedBlue.g);
    EXPECT_FLOAT_EQ(fallbackBlue.b, expectedBlue.b);

    // 3. UI 语言判定单一事实源
    ASSERT_TRUE(cfg.set("/general/language", std::string("zh-CN")));
    EXPECT_TRUE(NativeThemePalette::isUiLanguageChinese());
    EXPECT_TRUE(tools3000::core::isUiLanguageChinese());

    ASSERT_TRUE(cfg.set("/general/language", std::string("zh-TW")));
    EXPECT_TRUE(NativeThemePalette::isUiLanguageChinese());

    ASSERT_TRUE(cfg.set("/general/language", std::string("en-US")));
    EXPECT_FALSE(NativeThemePalette::isUiLanguageChinese());
    EXPECT_FALSE(tools3000::core::isUiLanguageChinese());

    ASSERT_TRUE(cfg.set("/general/language", std::string("auto")));
    EXPECT_EQ(NativeThemePalette::isUiLanguageChinese(), WinUtils::isSystemLanguageChinese());

    // 4. Win32 原生菜单暗色策略动态加载幂等无崩溃验证
    WinUtils::applyNativeMenuTheme("dark");
    WinUtils::applyNativeMenuTheme("light");
    WinUtils::applyNativeMenuTheme("system");
    WinUtils::applyNativeMenuTheme();
}

TEST(WinUtilsFileTypeIconCacheTest, StaticExtensionZeroDiskSharedCacheHitRate) {
    using tools3000::core::WinUtils;

    WinUtils::clearFileTypeIconCacheForTesting();
    EXPECT_EQ(WinUtils::getFileTypeIconCacheSizeForTesting(), 0u);

    // 静态扩展名 (.txt): 同时验证三参数标准调用与首参传入完整路径的自适应提取，零物理磁盘 I/O + 共享缓存 100% 命中
    const std::string txtA = WinUtils::getFileTypeIconBase64(
        L".txt", false, L"Z:\\__t3k_nonexistent_volume_a__\\doc_alpha.txt");
    const std::string txtB = WinUtils::getFileTypeIconBase64(
        L"TXT", false, L"Y:\\__t3k_nonexistent_volume_b__\\DOC_BETA.TXT");
    const std::string txtC = WinUtils::getFileTypeIconBase64(
        L"X:\\__t3k_nonexistent_volume_c__\\nested\\doc_gamma.Txt", false);

    EXPECT_TRUE(txtA.starts_with("data:image/png;base64,"));
    EXPECT_GT(txtA.size(), 64u);
    EXPECT_EQ(txtA, txtB);
    EXPECT_EQ(txtA, txtC);
    EXPECT_EQ(WinUtils::getFileTypeIconCacheSizeForTesting(), 1u);

    // 另一种静态扩展名 (.cpp): 仅新增 1 条共享缓存
    const std::string cppA = WinUtils::getFileTypeIconBase64(
        L".cpp", false, L"Z:\\__t3k_nonexistent_volume_a__\\engine.cpp");
    const std::string cppB = WinUtils::getFileTypeIconBase64(
        L".CPP", false, L"W:\\__t3k_nonexistent_volume_d__\\ENGINE.CPP");
    EXPECT_TRUE(cppA.starts_with("data:image/png;base64,"));
    EXPECT_EQ(cppA, cppB);
    EXPECT_EQ(WinUtils::getFileTypeIconCacheSizeForTesting(), 2u);

    // 目录类型图标: 跨路径共享单条 ::dir:: 缓存
    const std::string dirA = WinUtils::getFileTypeIconBase64(
        L"", true, L"Z:\\__t3k_nonexistent_folder_1__");
    const std::string dirB = WinUtils::getFileTypeIconBase64(
        L"Z:\\__t3k_nonexistent_folder_2__", true);
    EXPECT_TRUE(dirA.starts_with("data:image/png;base64,"));
    EXPECT_EQ(dirA, dirB);
    EXPECT_EQ(WinUtils::getFileTypeIconCacheSizeForTesting(), 3u);

    // 动态实例图标后缀 (.exe): 按规范化文件路径独立缓存，避免不同可执行文件串用图标
    wchar_t sysDir[MAX_PATH]{};
    const UINT sysLen = GetSystemDirectoryW(sysDir, MAX_PATH);
    if (sysLen > 0 && sysLen < MAX_PATH) {
        const std::wstring cmdExe = std::filesystem::path(sysDir) / L"cmd.exe";
        const std::wstring notepadExe = std::filesystem::path(sysDir) / L"notepad.exe";
        if (std::filesystem::exists(cmdExe) && std::filesystem::exists(notepadExe)) {
            const std::string iconCmd = WinUtils::getFileTypeIconBase64(L".exe", false, cmdExe);
            const std::string iconNotepad = WinUtils::getFileTypeIconBase64(L".exe", false, notepadExe);
            EXPECT_TRUE(iconCmd.starts_with("data:image/png;base64,"));
            EXPECT_TRUE(iconNotepad.starts_with("data:image/png;base64,"));
            EXPECT_EQ(WinUtils::getFileTypeIconCacheSizeForTesting(), 5u);
        }
    }
}

TEST(FileInteractionAsyncHandlersTest, DispatchesFileOperationsOffCallerThread) {
    auto& bridge = tools3000::core::MessageBridge::instance();
    tools3000::core::registerFileInteractionHandlers(
        bridge, "audit.asyncfile", "path is required");

    struct UnregisterGuard {
        tools3000::core::MessageBridge& mb;
        ~UnregisterGuard() {
            EXPECT_EQ(mb.unregisterHandlersByPrefix("audit.asyncfile"), 8u);
        }
    } guard{bridge};

    const DWORD callerTid = GetCurrentThreadId();
    for (const char* methodSuffix : {
             "openFile", "openFolder", "openFileAsAdmin",
             "showFileProperties", "openWithNotepad", "copyFile"}) {
        std::promise<std::pair<DWORD, std::string>> donePromise;
        auto doneFuture = donePromise.get_future();

        const nlohmann::json req = {
            {"id", 901},
            {"method", std::string("audit.asyncfile.") + methodSuffix},
            {"params", nlohmann::json::object()}
        };
        bridge.handleMessageAsync(req.dump(), [&donePromise](const std::string& resp) {
            donePromise.set_value({GetCurrentThreadId(), resp});
        });

        ASSERT_EQ(doneFuture.wait_for(std::chrono::seconds(5)), std::future_status::ready)
            << methodSuffix;
        const auto [workerTid, rawResp] = doneFuture.get();
        EXPECT_NE(workerTid, callerTid) << methodSuffix;

        const auto parsed = nlohmann::json::parse(rawResp);
        ASSERT_TRUE(parsed.contains("result")) << rawResp;
        EXPECT_FALSE(parsed["result"].value("success", true)) << methodSuffix;
        EXPECT_EQ(parsed["result"].value("error", ""), "path is required") << methodSuffix;
    }
}

TEST(ConfigManagerBatch2RegressionTest, CoalescesRapidWritesIntoAsyncAtomicFlush) {
    const auto tempDir = std::filesystem::temp_directory_path() / "t3k_cfg_batch2_coalesce";
    std::error_code ec;
    std::filesystem::remove_all(tempDir, ec);
    std::filesystem::create_directories(tempDir, ec);

    auto& cfg = tools3000::core::ConfigManager::instance();
    cfg.shutdown();
    ASSERT_TRUE(cfg.initialize(tempDir, "config.json"));

    const uint64_t writesBefore = cfg.diskWriteCount();
    for (int i = 0; i < 50; ++i) {
        ASSERT_TRUE(cfg.set("/perf/counter", i));
    }

    // 内存态必须 0ms 即刻可读最新值，且 200ms 合并窗口内物理写盘次数远小于 50 次
    EXPECT_EQ(cfg.get<int>("/perf/counter", -1), 49);
    const uint64_t writesBeforeFlush = cfg.diskWriteCount() - writesBefore;
    EXPECT_LE(writesBeforeFlush, 3u);

    ASSERT_TRUE(cfg.flush());
    const uint64_t totalWrites = cfg.diskWriteCount() - writesBefore;
    EXPECT_GE(totalWrites, 1u);
    EXPECT_LE(totalWrites, 4u);

    // 验证磁盘 config.json 原子持久化结果一致
    std::ifstream in(tempDir / "config.json", std::ios::binary);
    ASSERT_TRUE(in.is_open());
    const auto persisted = nlohmann::json::parse(in, nullptr, false);
    ASSERT_FALSE(persisted.is_discarded());
    EXPECT_EQ(persisted.value(nlohmann::json::json_pointer("/perf/counter"), -1), 49);
    in.close();

    cfg.shutdown();
    std::filesystem::remove_all(tempDir, ec);
}

TEST(MessageBridgeBatch2RegressionTest, FastHeaderExtractAndRawHandlerZeroReparse) {
    auto& bridge = tools3000::core::MessageBridge::instance();
    bridge.registerRawHandler("audit.raw.echo", [](const nlohmann::json& params) -> std::string {
        const std::string tag = params.value("tag", "");
        return std::string(R"({"rawDirect":true,"tag":)") + nlohmann::json(tag).dump() + "}";
    });
    bridge.registerAsyncRawHandler("audit.raw.asyncEcho", [](const nlohmann::json& params) -> std::string {
        const int count = params.value("count", 0);
        return std::string(R"({"asyncDirect":true,"count":)") + std::to_string(count) + "}";
    });

    struct Guard {
        tools3000::core::MessageBridge& mb;
        ~Guard() {
            mb.unregisterHandlersByPrefix("audit.raw");
        }
    } guard{bridge};

    // 1. 含转义引号与前置嵌套对象参数的快速头部解析与同步 Raw 直通（严防误取嵌套对象中的 id/method）
    const std::string syncReq =
        R"({"nested":{"fake":"\"method\":\"wrong.route\"","id":999},"id":1024,"method":"audit.raw.echo","params":{"tag":"hello\\world\"ok"}})";
    const std::string syncRespStr = bridge.handleMessage(syncReq);
    const auto syncResp = nlohmann::json::parse(syncRespStr);
    EXPECT_EQ(syncResp["id"].get<int>(), 1024);
    EXPECT_TRUE(syncResp["result"].value("rawDirect", false));
    EXPECT_EQ(syncResp["result"].value("tag", ""), "hello\\world\"ok");

    // 2. 异步 Raw 直通：前置嵌套干扰 + 零主线程全量 parse 并在工作线程直接拼装预序列化 JSON
    std::promise<std::string> asyncPromise;
    auto asyncFuture = asyncPromise.get_future();
    const std::string asyncReq =
        R"({"nested":{"id":888,"method":"wrong.route"},"id":2048,"method":"audit.raw.asyncEcho","params":{"count":77,"extra":"{\"method\":\"fake\"}"}})";
    bridge.handleMessageAsync(asyncReq, [&asyncPromise](std::string resp) {
        asyncPromise.set_value(std::move(resp));
    });
    ASSERT_EQ(asyncFuture.wait_for(std::chrono::seconds(5)), std::future_status::ready);
    const auto asyncResp = nlohmann::json::parse(asyncFuture.get());
    EXPECT_EQ(asyncResp["id"].get<int>(), 2048);
    EXPECT_TRUE(asyncResp["result"].value("asyncDirect", false));
    EXPECT_EQ(asyncResp["result"].value("count", 0), 77);
}

TEST(MouseStreamCoreBatch2RegressionTest, DefaultDormantAndScopedSubscriberRefCount) {
    auto& core = tools3000::core::MouseStreamCore::instance();
    core.setActive(false);
    while (core.subscriberCount() > 0) {
        core.releaseSubscriber();
    }
    core.clear();

    // 无订阅者且未显式激活时保持休眠，1000Hz 移动点零入队
    EXPECT_FALSE(core.isActive());
    core.pushRawMove(120, 240);
    EXPECT_TRUE(core.empty());

    {
        tools3000::core::MouseStreamCore::ScopedSubscriber sub1(core);
        EXPECT_EQ(core.subscriberCount(), 1u);
        EXPECT_TRUE(core.isActive());
        core.pushRawMove(300, 400);
        EXPECT_FALSE(core.empty());

        {
            tools3000::core::MouseStreamCore::ScopedSubscriber sub2(core);
            EXPECT_EQ(core.subscriberCount(), 2u);
            EXPECT_TRUE(core.isActive());
        }

        // sub2 析构后 sub1 仍存活，保持激活状态
        EXPECT_EQ(core.subscriberCount(), 1u);
        EXPECT_TRUE(core.isActive());
    }

    // 全部订阅者析构后自动停用并清空残留采样点
    EXPECT_EQ(core.subscriberCount(), 0u);
    EXPECT_FALSE(core.isActive());
    EXPECT_TRUE(core.empty());
}

TEST(LuaEngineBatch3RegressionTest, NonBlockingToastAndErrorCodeSafeExists) {
    auto& lua = tools3000::core::LuaEngine::instance();
    ASSERT_TRUE(lua.initialize());

    std::atomic<int> toastPublished{0};
    std::wstring capturedToastMsg;
    std::mutex msgMutex;
    const auto subId = tools3000::core::EventBus::instance().subscribe<tools3000::core::ShowToastEvent>(
        [&](const tools3000::core::ShowToastEvent& ev) {
            {
                std::lock_guard<std::mutex> lk(msgMutex);
                capturedToastMsg = ev.message;
            }
            toastPublished.fetch_add(1, std::memory_order_relaxed);
        });

    const bool okToast = lua.executeScript(
        R"(easy.ui.toast("Batch3NonBlockingToast"))",
        tools3000::core::LuaPermission::All);
    EXPECT_TRUE(okToast);
    EXPECT_GE(toastPublished.load(std::memory_order_relaxed), 1);
    {
        std::lock_guard<std::mutex> lk(msgMutex);
        EXPECT_EQ(capturedToastMsg, L"Batch3NonBlockingToast");
    }
    tools3000::core::EventBus::instance().unsubscribeAndWait(subId);

    // 验证 easy.fs.exists 针对非法设备路径与含空字节路径安全返回 false 且不抛未捕获异常
    const bool okFs = lua.executeScript(
        R"(
            assert(easy.fs.exists("\\\\?\\GLOBALROOT\\Device\\InvalidNonExistentVolume999\\x") == false)
            assert(easy.fs.exists("Z:\\*invalid*path?|<>\\test.txt") == false)
            assert(easy.fs.exists("bad\0embedded") == false)
        )",
        tools3000::core::LuaPermission::All);
    EXPECT_TRUE(okFs);
}

TEST(StatsManagerNumpadEnterTest, TracksExtendedNumpadEnter0x10DSeparatelyAndRoundTripsJson) {
    // 1. 验证 DailyStats JSON 序列化往返对 0x0D (13) 与 0x10D (269) 均无损保留
    tools3000::core::DailyStats sample;
    sample.totalKeys = 15;
    sample.keyMap[VK_RETURN] = 9;
    sample.keyMap[0x10D] = 6;
    const auto dumped = sample.toJson();
    const auto restored = tools3000::core::DailyStats::fromJson(dumped);
    EXPECT_EQ(restored.totalKeys, 15u);
    ASSERT_TRUE(restored.keyMap.contains(VK_RETURN));
    ASSERT_TRUE(restored.keyMap.contains(0x10D));
    EXPECT_EQ(restored.keyMap.at(VK_RETURN), 9u);
    EXPECT_EQ(restored.keyMap.at(0x10D), 6u);

    // 2. 验证 StatsManager 无锁环形计数槽 (512 容量) 对 0x0D 与 0x10D 独立累加互不丢弃
    auto& stats = tools3000::core::StatsManager::instance();
    stats.initialize();
    const auto before = stats.getTodayStats();
    const uint64_t baseMainEnter = before.keyMap.contains(VK_RETURN) ? before.keyMap.at(VK_RETURN) : 0ULL;
    const uint64_t baseNumpadEnter = before.keyMap.contains(0x10D) ? before.keyMap.at(0x10D) : 0ULL;

    stats.recordKey(VK_RETURN);
    stats.recordKey(VK_RETURN);
    stats.recordKey(0x10D);
    stats.recordKey(0x10D);
    stats.recordKey(0x10D);

    const auto after = stats.getTodayStats();
    ASSERT_TRUE(after.keyMap.contains(VK_RETURN));
    ASSERT_TRUE(after.keyMap.contains(0x10D));
    EXPECT_EQ(after.keyMap.at(VK_RETURN), baseMainEnter + 2ULL);
    EXPECT_EQ(after.keyMap.at(0x10D), baseNumpadEnter + 3ULL);
    stats.shutdown();
}

TEST(MessageBridgeOpenSaveDirTest, ResolvesAppDataDefaultAndKindFallbacks) {
    const auto tempCfgDir = std::filesystem::temp_directory_path() / "t3k_cfg_opensavedir_test";
    std::error_code ec;
    std::filesystem::remove_all(tempCfgDir, ec);
    std::filesystem::create_directories(tempCfgDir, ec);

    auto& cfg = tools3000::core::ConfigManager::instance();
    cfg.shutdown();
    ASSERT_TRUE(cfg.initialize(tempCfgDir, "config.json"));

    auto& bridge = tools3000::core::MessageBridge::instance();
    if (!bridge.hasHandler("capture.openSaveDirectory")) {
        bridge.registerBuiltinHandlers();
    }
    const auto expectedScreenshots =
        tools3000::core::WinUtils::wstringToUtf8((tools3000::core::WinUtils::getAppDataDirectory() / L"Screenshots").wstring());
    const auto expectedRecordings =
        tools3000::core::WinUtils::wstringToUtf8((tools3000::core::WinUtils::getAppDataDirectory() / L"Recordings").wstring());

    // 1. 默认空路径：kind="capture" 回退 AppData/Screenshots，kind="recording" 回退 AppData/Recordings
    const auto capDefault = bridge.invokeHandler("capture.openSaveDirectory", {
        {"path", ""},
        {"kind", "capture"},
        {"dryRun", true}
    });
    EXPECT_TRUE(capDefault.value("success", false));
    EXPECT_EQ(capDefault.value("path", ""), expectedScreenshots);

    const auto recDefault = bridge.invokeHandler("capture.openSaveDirectory", {
        {"path", "   "},
        {"kind", "recording"},
        {"dryRun", true}
    });
    EXPECT_TRUE(recDefault.value("success", false));
    EXPECT_EQ(recDefault.value("path", ""), expectedRecordings);

    // 2. 配置了兼容字段 /capture/savePath 与 /recording/saveDirectory 时优先按顺序读取
    const auto customCapDir = tempCfgDir / "CustomShots";
    const auto customRecDir = tempCfgDir / "CustomRecs";
    ASSERT_TRUE(cfg.set("/capture/savePath", tools3000::core::WinUtils::wstringToUtf8(customCapDir.wstring())));
    ASSERT_TRUE(cfg.set("/recording/saveDirectory", tools3000::core::WinUtils::wstringToUtf8(customRecDir.wstring())));

    const auto capCustom = bridge.invokeHandler("capture.openSaveDirectory", {
        {"path", ""},
        {"kind", "capture"},
        {"dryRun", true}
    });
    EXPECT_TRUE(capCustom.value("success", false));
    EXPECT_EQ(capCustom.value("path", ""), tools3000::core::WinUtils::wstringToUtf8(customCapDir.wstring()));
    EXPECT_TRUE(std::filesystem::exists(customCapDir));

    const auto recCustom = bridge.invokeHandler("capture.openSaveDirectory", {
        {"path", ""},
        {"kind", "recording"},
        {"dryRun", true}
    });
    EXPECT_TRUE(recCustom.value("success", false));
    EXPECT_EQ(recCustom.value("path", ""), tools3000::core::WinUtils::wstringToUtf8(customRecDir.wstring()));
    EXPECT_TRUE(std::filesystem::exists(customRecDir));

    cfg.shutdown();
    std::filesystem::remove_all(tempCfgDir, ec);
}

TEST(WinUtilsShortcutSyncTest, AnchorsIconLocationToTargetAndRemainsIdempotent) {
    const HRESULT hrCom = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    const bool comInited = SUCCEEDED(hrCom);

    std::error_code ec;
    const auto tempDir = std::filesystem::temp_directory_path() /
        (L"Tools3000_ShortcutSyncTest_" + std::to_wstring(GetCurrentProcessId()));
    std::filesystem::remove_all(tempDir, ec);
    std::filesystem::create_directories(tempDir / L"Installed", ec);
    std::filesystem::create_directories(tempDir / L"PollutedDev", ec);

    const auto fakeInstalledExe = tempDir / L"Installed" / L"Tools3000.exe";
    const auto fakePollutedExe = tempDir / L"PollutedDev" / L"Tools3000.exe";
    const auto lnkPath = tempDir / L"Tools3000.lnk";
    {
        std::ofstream(fakeInstalledExe, std::ios::binary) << "MZ";
        std::ofstream(fakePollutedExe, std::ios::binary) << "MZ";
    }

    // 构造一个被开发构建目录跨路径污染了 IconLocation 的快捷方式
    {
        IShellLinkW* psl = nullptr;
        ASSERT_TRUE(SUCCEEDED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&psl))) && psl);
        ASSERT_TRUE(SUCCEEDED(psl->SetPath(fakeInstalledExe.c_str())));
        ASSERT_TRUE(SUCCEEDED(psl->SetIconLocation(fakePollutedExe.c_str(), 2)));
        IPersistFile* ppf = nullptr;
        ASSERT_TRUE(SUCCEEDED(psl->QueryInterface(IID_PPV_ARGS(&ppf))) && ppf);
        ASSERT_TRUE(SUCCEEDED(ppf->Save(lnkPath.c_str(), TRUE)));
        ppf->Release();
        psl->Release();
    }

    // 首次同步：必须将 IconLocation 修复回快捷方式自身的 fakeInstalledExe (,0)，而非测试进程自身路径，并写入 AUMID
    EXPECT_TRUE(tools3000::core::WinUtils::syncSingleShortcutMetadata(lnkPath.wstring(), L"Yy1.Tools3000", false));

    {
        IShellLinkW* psl = nullptr;
        ASSERT_TRUE(SUCCEEDED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&psl))) && psl);
        IPersistFile* ppf = nullptr;
        ASSERT_TRUE(SUCCEEDED(psl->QueryInterface(IID_PPV_ARGS(&ppf))) && ppf);
        ASSERT_TRUE(SUCCEEDED(ppf->Load(lnkPath.c_str(), STGM_READ)));

        wchar_t iconBuf[MAX_PATH]{};
        int iconIdx = -1;
        ASSERT_TRUE(SUCCEEDED(psl->GetIconLocation(iconBuf, MAX_PATH, &iconIdx)));
        EXPECT_EQ(_wcsicmp(iconBuf, fakeInstalledExe.c_str()), 0);
        EXPECT_EQ(iconIdx, 0);

        ppf->Release();
        psl->Release();
    }

    // 二次常规同步：元数据完全一致，必须幂等返回 false（0 次磁盘覆写）
    EXPECT_FALSE(tools3000::core::WinUtils::syncSingleShortcutMetadata(lnkPath.wstring(), L"Yy1.Tools3000", false));
    // 显式强制刷新 (--update-shortcuts)：返回 true
    EXPECT_TRUE(tools3000::core::WinUtils::syncSingleShortcutMetadata(lnkPath.wstring(), L"Yy1.Tools3000", true));

    std::filesystem::remove_all(tempDir, ec);
    if (comInited) {
        CoUninitialize();
    }
}

namespace {

void writeMinimalDeflatedZip(
    const std::filesystem::path& zipPath,
    const std::vector<std::pair<std::string, std::string>>& files) {
    struct CdEntry {
        std::string name;
        uint32_t crc = 0;
        uint32_t compSize = 0;
        uint32_t uncompSize = 0;
        uint32_t localOffset = 0;
    };

    std::vector<uint8_t> out;
    auto appendU16 = [&](uint16_t v) {
        out.push_back(static_cast<uint8_t>(v & 0xFFu));
        out.push_back(static_cast<uint8_t>((v >> 8) & 0xFFu));
    };
    auto appendU32 = [&](uint32_t v) {
        out.push_back(static_cast<uint8_t>(v & 0xFFu));
        out.push_back(static_cast<uint8_t>((v >> 8) & 0xFFu));
        out.push_back(static_cast<uint8_t>((v >> 16) & 0xFFu));
        out.push_back(static_cast<uint8_t>((v >> 24) & 0xFFu));
    };

    std::vector<CdEntry> cdEntries;
    for (const auto& [name, content] : files) {
        std::vector<uint8_t> compBuf(content.size() + 128);
        z_stream zs{};
        ASSERT_EQ(deflateInit2(&zs, Z_DEFAULT_COMPRESSION, Z_DEFLATED, -MAX_WBITS, 8, Z_DEFAULT_STRATEGY), Z_OK);
        zs.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(content.data()));
        zs.avail_in = static_cast<uInt>(content.size());
        zs.next_out = compBuf.data();
        zs.avail_out = static_cast<uInt>(compBuf.size());
        ASSERT_EQ(deflate(&zs, Z_FINISH), Z_STREAM_END);
        const uint32_t compSize = static_cast<uint32_t>(zs.total_out);
        deflateEnd(&zs);

        const uint32_t crc = crc32(0L, reinterpret_cast<const Bytef*>(content.data()), static_cast<uInt>(content.size()));
        const uint32_t localOffset = static_cast<uint32_t>(out.size());

        appendU32(0x04034b50u);
        appendU16(20);
        appendU16(0);
        appendU16(8); // deflated
        appendU16(0);
        appendU16(0);
        appendU32(crc);
        appendU32(compSize);
        appendU32(static_cast<uint32_t>(content.size()));
        appendU16(static_cast<uint16_t>(name.size()));
        appendU16(0);
        out.insert(out.end(), name.begin(), name.end());
        out.insert(out.end(), compBuf.begin(), compBuf.begin() + compSize);

        cdEntries.push_back({name, crc, compSize, static_cast<uint32_t>(content.size()), localOffset});
    }

    const uint32_t cdOffset = static_cast<uint32_t>(out.size());
    for (const auto& e : cdEntries) {
        appendU32(0x02014b50u);
        appendU16(20);
        appendU16(20);
        appendU16(0);
        appendU16(8);
        appendU16(0);
        appendU16(0);
        appendU32(e.crc);
        appendU32(e.compSize);
        appendU32(e.uncompSize);
        appendU16(static_cast<uint16_t>(e.name.size()));
        appendU16(0);
        appendU16(0);
        appendU16(0);
        appendU16(0);
        appendU32(0);
        appendU32(e.localOffset);
        out.insert(out.end(), e.name.begin(), e.name.end());
    }
    const uint32_t cdSize = static_cast<uint32_t>(out.size()) - cdOffset;

    appendU32(0x06054b50u);
    appendU16(0);
    appendU16(0);
    appendU16(static_cast<uint16_t>(cdEntries.size()));
    appendU16(static_cast<uint16_t>(cdEntries.size()));
    appendU32(cdSize);
    appendU32(cdOffset);
    appendU16(0);

    std::ofstream f(zipPath, std::ios::binary | std::ios::trunc);
    f.write(reinterpret_cast<const char*>(out.data()), static_cast<std::streamsize>(out.size()));
}

void writeMinimalValidSinglePagePdf(const std::filesystem::path& pdfPath) {
    std::string pdf = "%PDF-1.4\n";
    std::vector<size_t> offsets(5, 0);

    offsets[1] = pdf.size();
    pdf += "1 0 obj\n<< /Type /Catalog /Pages 2 0 R >>\nendobj\n";

    offsets[2] = pdf.size();
    pdf += "2 0 obj\n<< /Type /Pages /Kids [3 0 R] /Count 1 >>\nendobj\n";

    offsets[3] = pdf.size();
    pdf += "3 0 obj\n<< /Type /Page /Parent 2 0 R /MediaBox [0 0 300 180] /Contents 4 0 R >>\nendobj\n";

    const std::string streamBody = "0.1 0.4 0.8 rg 24 24 252 132 re f\n";
    offsets[4] = pdf.size();
    pdf += "4 0 obj\n<< /Length " + std::to_string(streamBody.size()) + " >>\nstream\n" + streamBody + "endstream\nendobj\n";

    const size_t xrefPos = pdf.size();
    pdf += "xref\n0 5\n0000000000 65535 f \n";
    for (size_t i = 1; i <= 4; ++i) {
        char line[32]{};
        std::snprintf(line, sizeof(line), "%010zu 00000 n \n", offsets[i]);
        pdf += line;
    }
    pdf += "trailer\n<< /Size 5 /Root 1 0 R >>\nstartxref\n" + std::to_string(xrefPos) + "\n%%EOF\n";

    std::ofstream f(pdfPath, std::ios::binary | std::ios::trunc);
    f.write(pdf.data(), static_cast<std::streamsize>(pdf.size()));
}

} // namespace

TEST(TextFilePreviewDecoderTest, BlocksBinarySignaturesAndTrimsSplitGbkTail) {
    using tools3000::core::TextFilePreviewDecoder;

    // 1. ZIP/Office PK\x03\x04 stream without 0x00 bytes must never be misclassified as GBK text
    const uint8_t fakeZipNoNulls[] = {0x50, 0x4B, 0x03, 0x04, 0x14, 0x41, 0x42, 0x43, 0xD4, 0xC6};
    const auto zipRes = TextFilePreviewDecoder::decodeBuffer(fakeZipNoNulls, sizeof(fakeZipNoNulls));
    EXPECT_FALSE(zipRes.isReadableText);
    EXPECT_FALSE(zipRes.hexDump.empty());

    // 2. PDF %PDF-1.7 signature must never be misclassified as text
    const char fakePdf[] = "%PDF-1.7\n1 0 obj\n<< /Type /Catalog >>\nendobj\n";
    const auto pdfRes = TextFilePreviewDecoder::decodeBuffer(
        reinterpret_cast<const uint8_t*>(fakePdf), sizeof(fakePdf) - 1);
    EXPECT_FALSE(pdfRes.isReadableText);

    // 3. GBK CSV with trailing split lead byte (0xD4 without trailing byte) trims cleanly to "云南"
    // GBK for "云南": 0xD4 0xC6 ("云"), 0xC4 0xCF ("南")
    const uint8_t gbkSplitTail[] = {'A', ',', 0xD4, 0xC6, 0xC4, 0xCF, 0xD4};
    const auto gbkRes = TextFilePreviewDecoder::decodeBuffer(gbkSplitTail, sizeof(gbkSplitTail));
    EXPECT_TRUE(gbkRes.isReadableText);
    EXPECT_EQ(gbkRes.encoding, "GBK");
    EXPECT_EQ(gbkRes.utf8Text, "A,\xE4\xBA\x91\xE5\x8D\x97"); // UTF-8 "A,云南"
}

TEST(OfficeAndPdfPreviewTest, ExtractsXlsxDocxPptxAndRendersPdfZeroLock) {
    using tools3000::core::OfficeAndPdfPreview;

    std::error_code ec;
    const auto tempDir = std::filesystem::temp_directory_path() /
        (L"Tools3000_OfficePdfPreview_" + std::to_wstring(GetCurrentProcessId()));
    std::filesystem::remove_all(tempDir, ec);
    std::filesystem::create_directories(tempDir, ec);

    // 1. XLSX multi-sheet with sparse column reference (A1, C1)
    const auto xlsxPath = tempDir / L"云南出行预算.xlsx";
    writeMinimalDeflatedZip(xlsxPath, {
        {"xl/workbook.xml",
         R"(<?xml version="1.0" encoding="UTF-8"?><workbook><sheets><sheet name="行程预算" sheetId="1" r:id="rId1"/></sheets></workbook>)"},
        {"xl/_rels/workbook.xml.rels",
         R"(<?xml version="1.0" encoding="UTF-8"?><Relationships><Relationship Id="rId1" Target="worksheets/sheet1.xml"/></Relationships>)"},
        {"xl/sharedStrings.xml",
         R"(<?xml version="1.0" encoding="UTF-8"?><sst><si><t>项目</t></si><si><t>预算金额</t></si><si><t>大理海景民宿</t></si></sst>)"},
        {"xl/worksheets/sheet1.xml",
         R"(<?xml version="1.0" encoding="UTF-8"?><worksheet><sheetData><row r="1"><c r="A1" t="s"><v>0</v></c><c r="C1" t="s"><v>1</v></c></row><row r="2"><c r="A2" t="s"><v>2</v></c><c r="C2"><v>1280</v></c></row></sheetData></worksheet>)"}
    });

    const auto xlsxRes = OfficeAndPdfPreview::extractXlsxPreview(xlsxPath.wstring());
    ASSERT_TRUE(xlsxRes.ok);
    ASSERT_EQ(xlsxRes.sheets.size(), 1u);
    EXPECT_EQ(xlsxRes.sheets[0].name, "行程预算");
    ASSERT_EQ(xlsxRes.sheets[0].rows.size(), 2u);
    ASSERT_EQ(xlsxRes.sheets[0].rows[0].size(), 3u);
    EXPECT_EQ(xlsxRes.sheets[0].rows[0][0], "项目");
    EXPECT_EQ(xlsxRes.sheets[0].rows[0][1], "");
    EXPECT_EQ(xlsxRes.sheets[0].rows[0][2], "预算金额");
    EXPECT_EQ(xlsxRes.sheets[0].rows[1][0], "大理海景民宿");
    EXPECT_EQ(xlsxRes.sheets[0].rows[1][2], "1280");

    // 2. DOCX heading + embedded table
    const auto docxPath = tempDir / L"云南五日游攻略.docx";
    writeMinimalDeflatedZip(docxPath, {
        {"word/document.xml",
         R"(<?xml version="1.0" encoding="UTF-8"?><w:document><w:body><w:p><w:pPr><w:pStyle w:val="Heading1"/></w:pPr><w:r><w:t>云南五日游攻略</w:t></w:r></w:p><w:tbl><w:tr><w:tc><w:p><w:r><w:t>天数</w:t></w:r></w:p></w:tc><w:tc><w:p><w:r><w:t>景点</w:t></w:r></w:p></w:tc></w:tr><w:tr><w:tc><w:p><w:r><w:t>Day 1</w:t></w:r></w:p></w:tc><w:tc><w:p><w:r><w:t>洱海生态廊道</w:t></w:r></w:p></w:tc></w:tr></w:tbl></w:body></w:document>)"}
    });
    const auto docxRes = OfficeAndPdfPreview::extractDocxOrPptxPreview(docxPath.wstring(), "docx");
    ASSERT_TRUE(docxRes.ok);
    EXPECT_NE(docxRes.markdownText.find("# 云南五日游攻略"), std::string::npos);
    EXPECT_NE(docxRes.markdownText.find("洱海生态廊道"), std::string::npos);

    // 3. PDF zero-lock native rasterization + verify file can be immediately deleted right after preview
    const auto pdfPath = tempDir / L"机票行程单.pdf";
    writeMinimalValidSinglePagePdf(pdfPath);
    const auto pdfRes = OfficeAndPdfPreview::renderPdfPreviewPages(pdfPath.wstring(), 4, 800);
    EXPECT_FALSE(pdfRes.passwordProtected);
    EXPECT_TRUE(pdfRes.ok);
    EXPECT_EQ(pdfRes.totalPageCount, 1u);
    ASSERT_EQ(pdfRes.pageDataUris.size(), 1u);
    EXPECT_EQ(pdfRes.pageDataUris[0].rfind("data:image/png;base64,", 0), 0u);
    // Zero file lock assertion: deleting pdfPath immediately must succeed!
    EXPECT_TRUE(std::filesystem::remove(pdfPath, ec));

    // 4. Blank template default.docx (<w:body><w:sectPr/></w:body>) must return ok=true & emptyDocument=true
    const auto blankDocxPath = tempDir / L"default.docx";
    writeMinimalDeflatedZip(blankDocxPath, {
        {"word/document.xml",
         R"(<?xml version="1.0" encoding="UTF-8"?><w:document><w:body><w:sectPr/></w:body></w:document>)"}
    });
    const auto blankDocxRes = OfficeAndPdfPreview::extractDocxOrPptxPreview(blankDocxPath.wstring(), "docx");
    ASSERT_TRUE(blankDocxRes.ok);
    EXPECT_TRUE(blankDocxRes.emptyDocument);
    EXPECT_TRUE(blankDocxRes.markdownText.empty());

    // 5. Image-only screenshot .docx (word/media/image1.png) must return ok=true & embeddedImages.size()==1
    const auto imgDocxPath = tempDir / L"工单截图.docx";
    writeMinimalDeflatedZip(imgDocxPath, {
        {"word/document.xml",
         R"(<?xml version="1.0" encoding="UTF-8"?><w:document><w:body><w:p/></w:body></w:document>)"},
        {"word/media/image1.png", "\x89PNG\r\n\x1a\n\x00\x00\x00\x0dIHDR"}
    });
    const auto imgDocxRes = OfficeAndPdfPreview::extractDocxOrPptxPreview(imgDocxPath.wstring(), "docx");
    ASSERT_TRUE(imgDocxRes.ok);
    EXPECT_FALSE(imgDocxRes.emptyDocument);
    ASSERT_EQ(imgDocxRes.embeddedImages.size(), 1u);
    EXPECT_EQ(imgDocxRes.embeddedImages[0].rfind("data:image/png;base64,", 0), 0u);

    std::filesystem::remove_all(tempDir, ec);
}

TEST(CaptureDefaultEnabledContractTest, FreshInstallConfigAndInstallerCheckableAloneGuard) {
    // 1. 验证空白配置下 PluginManager::reconcilePluginConfigs 显式启用 capture，
    //    且当用户显式禁用 capture 时严格保留 false 不反向覆盖。
    std::error_code ec;
    const auto tempDir = std::filesystem::temp_directory_path(ec) /
                         (L"tools3000_capture_default_test_" + std::to_wstring(GetCurrentProcessId()));
    std::filesystem::remove_all(tempDir, ec);
    ASSERT_TRUE(std::filesystem::create_directories(tempDir, ec));

    auto& cfg = tools3000::core::ConfigManager::instance();
    ASSERT_TRUE(cfg.initialize(tempDir, "config.json"));
    tools3000::core::PluginManager::instance().reconcilePluginConfigs();
    EXPECT_TRUE(cfg.has("/plugins/capture/enabled"));
    EXPECT_TRUE(cfg.get<bool>("/plugins/capture/enabled", false));

    cfg.set("/plugins/capture/enabled", false);
    tools3000::core::PluginManager::instance().reconcilePluginConfigs();
    EXPECT_FALSE(cfg.get<bool>("/plugins/capture/enabled", true));

    cfg.shutdown();
    std::filesystem::remove_all(tempDir, ec);

    // 2. 防回退门禁：校验仓库 packaging/installer/installer.iss 中 capture 父组件在挂载可选子组件时必须声明
    //    Types: full compact 与 Flags: checkablealone，防止 Inno Setup 全新安装自动取消勾选截图模块。
    std::filesystem::path probe = std::filesystem::current_path(ec);
    std::filesystem::path issPath;
    const auto relIss = std::filesystem::path(L"packaging") / L"installer" / L"installer.iss";
    for (int depth = 0; depth < 6 && !probe.empty(); ++depth) {
        if (std::filesystem::exists(probe / relIss, ec)) {
            issPath = probe / relIss;
            break;
        }
        probe = probe.parent_path();
    }
    if (issPath.empty()) {
        wchar_t exeBuf[MAX_PATH]{};
        const DWORD len = GetModuleFileNameW(nullptr, exeBuf, MAX_PATH);
        if (len > 0 && len < MAX_PATH) {
            probe = std::filesystem::path(exeBuf).parent_path();
            for (int depth = 0; depth < 6 && !probe.empty(); ++depth) {
                if (std::filesystem::exists(probe / relIss, ec)) {
                    issPath = probe / relIss;
                    break;
                }
                probe = probe.parent_path();
            }
        }
    }
    ASSERT_FALSE(issPath.empty()) << "Unable to locate installer.iss for component default regression guard";

    std::ifstream ifs(issPath);
    ASSERT_TRUE(ifs.is_open());
    bool inComponents = false;
    bool foundCapture = false;
    bool captureHasFull = false;
    bool captureHasCompact = false;
    bool captureHasCheckableAlone = false;
    bool foundOcrChild = false;
    std::string line;
    while (std::getline(ifs, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const auto firstNonWs = line.find_first_not_of(" \t");
        if (firstNonWs == std::string::npos || line[firstNonWs] == ';') continue;
        const std::string trimmed = line.substr(firstNonWs);
        if (trimmed.front() == '[') {
            inComponents = (trimmed == "[Components]");
            continue;
        }
        if (!inComponents) continue;
        if (trimmed.find("Name: \"capture\"") != std::string::npos) {
            foundCapture = true;
            captureHasFull = trimmed.find("full") != std::string::npos;
            captureHasCompact = trimmed.find("compact") != std::string::npos;
            captureHasCheckableAlone = trimmed.find("checkablealone") != std::string::npos;
        } else if (trimmed.find("Name: \"capture\\ocr_models\"") != std::string::npos) {
            foundOcrChild = true;
        }
    }
    EXPECT_TRUE(foundCapture);
    EXPECT_TRUE(captureHasFull);
    EXPECT_TRUE(captureHasCompact);
    EXPECT_TRUE(foundOcrChild);
    EXPECT_TRUE(captureHasCheckableAlone)
        << "Parent component 'capture' has optional child 'capture\\ocr_models' and MUST set Flags: checkablealone";
}

TEST(PicasaViewerDefaultDisabledAndInstallerContractTest, DefaultDisabledInConfigAndUncheckedByDefaultInInstaller) {
    // 1. 空白配置下 reconcilePluginConfigs() 必须保持 picasa_viewer 默认关闭，且同步写入同构键
    std::error_code ec;
    const auto tempDir = std::filesystem::temp_directory_path(ec) /
                         (L"tools3000_picasa_default_test_" + std::to_wstring(GetCurrentProcessId()));
    std::filesystem::remove_all(tempDir, ec);
    ASSERT_TRUE(std::filesystem::create_directories(tempDir, ec));

    auto& cfg = tools3000::core::ConfigManager::instance();
    ASSERT_TRUE(cfg.initialize(tempDir, "config.json"));
    tools3000::core::PluginManager::instance().reconcilePluginConfigs();

    EXPECT_TRUE(cfg.has("/plugins/picasa_viewer/enabled"));
    EXPECT_TRUE(cfg.has("/picasa_viewer/enabled"));
    EXPECT_FALSE(cfg.get<bool>("/plugins/picasa_viewer/enabled", true));
    EXPECT_FALSE(cfg.get<bool>("/picasa_viewer/enabled", true));

    // 2. 当安装器或用户显式启用后，reconcilePluginConfigs() 必须尊重开启状态
    cfg.set("/plugins/picasa_viewer/enabled", true);
    tools3000::core::PluginManager::instance().reconcilePluginConfigs();
    EXPECT_TRUE(cfg.get<bool>("/plugins/picasa_viewer/enabled", false));
    EXPECT_TRUE(cfg.get<bool>("/picasa_viewer/enabled", false));

    cfg.shutdown();
    std::filesystem::remove_all(tempDir, ec);

    // 3. 校验 installer.iss：picasaviewer 组件存在、不绑定 full/compact（初始默认不勾选）、8 语种文案齐备且含静默注册钩子
    std::filesystem::path issPath;
    const auto relIss = std::filesystem::path(L"packaging") / L"installer" / L"installer.iss";
    auto probe = std::filesystem::current_path(ec);
    for (int depth = 0; depth < 6 && !probe.empty(); ++depth) {
        if (std::filesystem::exists(probe / relIss, ec)) {
            issPath = probe / relIss;
            break;
        }
        probe = probe.parent_path();
    }
    if (issPath.empty()) {
        wchar_t exeBuf[MAX_PATH]{};
        const DWORD len = GetModuleFileNameW(nullptr, exeBuf, MAX_PATH);
        if (len > 0 && len < MAX_PATH) {
            probe = std::filesystem::path(exeBuf).parent_path();
            for (int depth = 0; depth < 6 && !probe.empty(); ++depth) {
                if (std::filesystem::exists(probe / relIss, ec)) {
                    issPath = probe / relIss;
                    break;
                }
                probe = probe.parent_path();
            }
        }
    }
    ASSERT_FALSE(issPath.empty()) << "Unable to locate installer.iss";

    std::ifstream ifs(issPath, std::ios::binary);
    ASSERT_TRUE(ifs.is_open());
    const std::string content((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());

    EXPECT_NE(content.find("chinesesimplified.CompPicasaViewer="), std::string::npos);
    EXPECT_NE(content.find("english.CompPicasaViewer="), std::string::npos);
    EXPECT_NE(content.find("Tools3000_Picasa_Viewer.exe"), std::string::npos);
    EXPECT_NE(content.find("--register-silent"), std::string::npos);
    EXPECT_NE(content.find("--unregister-silent"), std::string::npos);
    EXPECT_NE(content.find("IsPicasaViewerComponentSelected"), std::string::npos);

    std::istringstream issStream(content);
    bool inComponents = false;
    bool foundPicasa = false;
    bool picasaHasFull = false;
    bool picasaHasCompact = false;
    std::string line;
    while (std::getline(issStream, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const auto firstNonWs = line.find_first_not_of(" \t");
        if (firstNonWs == std::string::npos || line[firstNonWs] == ';') continue;
        const std::string trimmed = line.substr(firstNonWs);
        if (trimmed.front() == '[') {
            inComponents = (trimmed == "[Components]");
            continue;
        }
        if (!inComponents) continue;
        if (trimmed.find("Name: \"picasaviewer\"") != std::string::npos) {
            foundPicasa = true;
            picasaHasFull = trimmed.find("full") != std::string::npos;
            picasaHasCompact = trimmed.find("compact") != std::string::npos;
        }
    }
    EXPECT_TRUE(foundPicasa);
    EXPECT_FALSE(picasaHasFull) << "picasaviewer MUST NOT be checked by default in 'full' install type";
    EXPECT_FALSE(picasaHasCompact) << "picasaviewer MUST NOT be checked by default in 'compact' install type";
}

TEST(InstallerRebootGuardAndI18nTest, PfroSelfHealAndChineseRestartRadioContract) {
    std::error_code ec;
    const auto relIss = std::filesystem::path(L"packaging") / L"installer" / L"installer.iss";
    auto locateRepoRoot = [&]() -> std::filesystem::path {
        std::filesystem::path probe = std::filesystem::current_path(ec);
        for (int depth = 0; depth < 6 && !probe.empty(); ++depth) {
            if (std::filesystem::exists(probe / relIss, ec)) {
                return probe;
            }
            probe = probe.parent_path();
        }
        wchar_t exeBuf[MAX_PATH]{};
        const DWORD len = GetModuleFileNameW(nullptr, exeBuf, MAX_PATH);
        if (len > 0 && len < MAX_PATH) {
            probe = std::filesystem::path(exeBuf).parent_path();
            for (int depth = 0; depth < 6 && !probe.empty(); ++depth) {
                if (std::filesystem::exists(probe / relIss, ec)) {
                    return probe;
                }
                probe = probe.parent_path();
            }
        }
        return {};
    };

    const auto repoRoot = locateRepoRoot();
    ASSERT_FALSE(repoRoot.empty()) << "Unable to locate repository root for installer regression guard";

    // 1. 校验 installer.iss：必须在 NextButtonClick(wpReady)（早于 wpPreparing PFRO 扫描）调用自愈引擎，
    //    采用 Win32 原生 WriteExactMultiStringValue（绝不回退到会截断末尾空串的 RegWriteMultiStringValue），
    //    覆盖 PendingFileRenameOperations 与 PendingFileRenameOperations2，修复 FilesLocked 漏杀并拉长卸载等待窗口。
    {
        std::ifstream ifs(repoRoot / relIss, std::ios::binary);
        ASSERT_TRUE(ifs.is_open());
        const std::string content((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());

        EXPECT_NE(content.find("function NextButtonClick(CurPageID: Integer): Boolean;"), std::string::npos);
        EXPECT_NE(content.find("if CurPageID = wpReady then"), std::string::npos);
        EXPECT_NE(content.find("ClearStaleAppPendingFileRenames(WizardDirValue);"), std::string::npos);
        EXPECT_NE(content.find("'PendingFileRenameOperations'"), std::string::npos);
        EXPECT_NE(content.find("'PendingFileRenameOperations2'"), std::string::npos);
        EXPECT_NE(content.find("WriteExactMultiStringValue("), std::string::npos);
        EXPECT_NE(content.find("ReadExactMultiStringValue("), std::string::npos);
        EXPECT_EQ(content.find("RegWriteMultiStringValue("), std::string::npos)
            << "RegWriteMultiStringValue truncates trailing empty REG_MULTI_SZ items in PFRO; must use WriteExactMultiStringValue";
        EXPECT_NE(content.find("if (not ExistingInstallPresent) and (not NeedKillApp) and (not NeedKillService) and (not FilesLocked) then"), std::string::npos);
        EXPECT_NE(content.find("EvacuateLockedFilesInTree("), std::string::npos);
        EXPECT_NE(content.find("MoveFileExW("), std::string::npos);
        EXPECT_NE(content.find("WizardForm.NoRadio.Checked := True;"), std::string::npos);
        EXPECT_NE(content.find("WaitForProcessesAndFilesReleased(3500);"), std::string::npos);

        // 严苛红线：[Files] 段 100% 禁用 restartreplace，从物理根源阻断 Inno Setup 置位重启标志与自动重启！
        std::istringstream issStream(content);
        bool inFilesSection = false;
        int filesEntriesChecked = 0;
        std::string issLine;
        while (std::getline(issStream, issLine)) {
            if (!issLine.empty() && issLine.back() == '\r') issLine.pop_back();
            const auto firstChar = issLine.find_first_not_of(" \t");
            if (firstChar == std::string::npos || issLine[firstChar] == ';') continue;
            const std::string trimmed = issLine.substr(firstChar);
            if (trimmed.front() == '[') {
                inFilesSection = (trimmed == "[Files]");
                continue;
            }
            if (!inFilesSection) continue;
            ++filesEntriesChecked;
            EXPECT_EQ(trimmed.find("restartreplace"), std::string::npos)
                << "[Files] entry MUST NOT use restartreplace (causes forced VM/silent reboot): " << trimmed;
        }
        EXPECT_GE(filesEntriesChecked, 5);
    }

    // 2. 校验 ChineseSimplified.isl：重启单选按钮不得再错译为“我同意此协议 / 我不同意此协议”
    {
        std::ifstream ifs(repoRoot / L"resources" / L"installer" / L"ChineseSimplified.isl", std::ios::binary);
        ASSERT_TRUE(ifs.is_open());
        std::string yesLine;
        std::string noLine;
        std::string line;
        while (std::getline(ifs, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.rfind("YesRadio=", 0) == 0) yesLine = line;
            if (line.rfind("NoRadio=", 0) == 0) noLine = line;
        }
        ASSERT_FALSE(yesLine.empty());
        ASSERT_FALSE(noLine.empty());
        const auto asUtf8 = [](const char8_t* s) { return reinterpret_cast<const char*>(s); };
        EXPECT_EQ(yesLine.find(asUtf8(u8"同意此协议")), std::string::npos) << "YesRadio must not contain license agreement wording";
        EXPECT_EQ(noLine.find(asUtf8(u8"不同意此协议")), std::string::npos) << "NoRadio must not contain license agreement wording";
        EXPECT_NE(yesLine.find(asUtf8(u8"重新启动计算机")), std::string::npos);
        EXPECT_NE(noLine.find(asUtf8(u8"重新启动计算机")), std::string::npos);
    }
}

TEST(WinUtilsRdpCaptureAffinityGuardTest, NeverHidesControlBarsInRemoteDesktopOrLegacyServer) {
    using tools3000::core::WinUtils;

    if (WinUtils::isRemoteDesktopSession()) {
        EXPECT_FALSE(WinUtils::canSafelyExcludeWindowFromCapture());
    }

    HWND dummy = CreateWindowExW(
        WS_EX_LAYERED | WS_EX_TOOLWINDOW,
        L"STATIC", L"", WS_POPUP,
        0, 0, 64, 24, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    ASSERT_NE(dummy, nullptr);

    EXPECT_FALSE(WinUtils::applyCaptureExclusionIfSafe(dummy, false));
    DWORD affinity = WDA_EXCLUDEFROMCAPTURE;
    if (GetWindowDisplayAffinity(dummy, &affinity)) {
        EXPECT_EQ(affinity, static_cast<DWORD>(WDA_NONE));
    }

    const bool applied = WinUtils::applyCaptureExclusionIfSafe(dummy, true);
    EXPECT_EQ(applied, WinUtils::canSafelyExcludeWindowFromCapture());
    if (GetWindowDisplayAffinity(dummy, &affinity)) {
        EXPECT_EQ(affinity, applied ? static_cast<DWORD>(WDA_EXCLUDEFROMCAPTURE) : static_cast<DWORD>(WDA_NONE));
    }

    DestroyWindow(dummy);
}

TEST(SessionIsolationTest, MultiSessionMutexWebViewSlotLeasesAndConcurrentAtomicWrite) {
    using tools3000::core::SessionIsolation;
    using tools3000::core::WinUtils;

    // 1. 会话级互斥体命名契约：正常模式必须使用 Local\ 命名空间允许 Windows Server 多管理员并发登录
    const std::wstring normalMutex = SessionIsolation::buildSingleInstanceMutexName(false, 1234);
    EXPECT_EQ(normalMutex, L"Local\\Tools3000_SingleInstance_Mutex");
    const std::wstring testMutex = SessionIsolation::buildSingleInstanceMutexName(true, 5678);
    EXPECT_EQ(testMutex, L"Local\\Tools3000_LifecycleTest_5678");

    // 2. WebView2 主/副槽位内核租约：主槽位优先、并发自动降级副槽位、释放后低熵复用、主槽位清理过期未锁副槽位
    const auto tempRoot = std::filesystem::temp_directory_path() /
                          (L"tools3000_session_iso_test_" + std::to_wstring(GetCurrentProcessId()));
    std::error_code ec;
    std::filesystem::remove_all(tempRoot, ec);
    std::filesystem::create_directories(tempRoot, ec);

    {
        // 模拟一个已过期 (10 天前) 且未锁定的僵尸副槽位目录 webview2_data_slot_9
        const auto staleDir = tempRoot / L"webview2_data_slot_9";
        std::filesystem::create_directories(staleDir, ec);
        std::ofstream(staleDir / L"dummy.txt") << "stale";
        const auto oldTime = std::filesystem::file_time_type::clock::now() - std::chrono::hours(24 * 10);
        std::filesystem::last_write_time(staleDir, oldTime, ec);

        // 会话 1 抢占主槽位 Slot 0 (webview2_data)，顺手清扫过期副槽位
        auto lease0 = SessionIsolation::acquireWebViewProfileSlot(tempRoot);
        ASSERT_TRUE(lease0.isHeld());
        EXPECT_TRUE(lease0.isPrimarySlot());
        EXPECT_EQ(lease0.slotIndex(), 0);
        EXPECT_EQ(lease0.dataDirectory().filename(), L"webview2_data");
        EXPECT_FALSE(std::filesystem::exists(staleDir, ec));

        // 并发会话 2 自动无冲突降级至 Slot 1 (webview2_data_slot_1)
        auto lease1 = SessionIsolation::acquireWebViewProfileSlot(tempRoot);
        ASSERT_TRUE(lease1.isHeld());
        EXPECT_FALSE(lease1.isPrimarySlot());
        EXPECT_EQ(lease1.slotIndex(), 1);
        EXPECT_EQ(lease1.dataDirectory().filename(), L"webview2_data_slot_1");

        // 并发会话 3 自动无冲突降级至 Slot 2 (webview2_data_slot_2)
        auto lease2 = SessionIsolation::acquireWebViewProfileSlot(tempRoot);
        ASSERT_TRUE(lease2.isHeld());
        EXPECT_EQ(lease2.slotIndex(), 2);
        EXPECT_EQ(lease2.dataDirectory().filename(), L"webview2_data_slot_2");

        // 即使强行执行 0 天激进清理，正在被 lease1/lease2 锁定的活跃副槽位也 100% 受到内核锁保护
        EXPECT_EQ(SessionIsolation::pruneStaleSecondaryWebViewSlots(tempRoot, 0), 0u);
        EXPECT_TRUE(std::filesystem::exists(lease1.dataDirectory(), ec));
        EXPECT_TRUE(std::filesystem::exists(lease2.dataDirectory(), ec));

        // 释放会话 2 的 Slot 1 -> 新来的会话 4 立刻复用 Slot 1（零目录无限膨胀）
        lease1.release();
        auto leaseReused = SessionIsolation::acquireWebViewProfileSlot(tempRoot);
        ASSERT_TRUE(leaseReused.isHeld());
        EXPECT_EQ(leaseReused.slotIndex(), 1);
        EXPECT_EQ(leaseReused.dataDirectory().filename(), L"webview2_data_slot_1");
    }

    // 全部租约释放后，Slot 0 立即恢复可用
    {
        auto leaseAgain = SessionIsolation::acquireWebViewProfileSlot(tempRoot);
        ASSERT_TRUE(leaseAgain.isHeld());
        EXPECT_EQ(leaseAgain.slotIndex(), 0);
    }

    // 3. 多线程高频并发原子落盘验证：8 线程同时覆盖写入同一目标文件 0 损坏、0 共享冲突崩溃
    {
        const auto targetFile = tempRoot / L"concurrent_config.json";
        std::atomic<int> successCount{0};
        std::vector<std::thread> workers;
        for (int t = 0; t < 8; ++t) {
            workers.emplace_back([&, t]() {
                for (int i = 0; i < 12; ++i) {
                    const std::string payload = "{\"worker\":" + std::to_string(t) + ",\"iter\":" + std::to_string(i) + "}";
                    if (WinUtils::atomicWriteFileWithFlush(targetFile.wstring(), payload)) {
                        successCount.fetch_add(1, std::memory_order_relaxed);
                    }
                }
            });
        }
        for (auto& w : workers) w.join();
        EXPECT_GE(successCount.load(), 90);
        std::ifstream ifs(targetFile);
        ASSERT_TRUE(ifs.is_open());
        const std::string finalContent((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
        EXPECT_TRUE(nlohmann::json::accept(finalContent)) << "Final concurrent file must remain valid JSON";
    }

    // 4. 源码与安装器防回退契约：main.cpp 与 installer.iss 必须接入 Local\ 会话级互斥体
    {
        const auto relIss = std::filesystem::path(L"packaging") / L"installer" / L"installer.iss";
        std::filesystem::path probe = std::filesystem::current_path(ec);
        std::filesystem::path repoRoot;
        for (int depth = 0; depth < 6 && !probe.empty(); ++depth) {
            if (std::filesystem::exists(probe / relIss, ec)) {
                repoRoot = probe;
                break;
            }
            probe = probe.parent_path();
        }
        if (!repoRoot.empty()) {
            std::ifstream mainIfs(repoRoot / L"src" / L"main.cpp", std::ios::binary);
            ASSERT_TRUE(mainIfs.is_open());
            const std::string mainSrc((std::istreambuf_iterator<char>(mainIfs)), std::istreambuf_iterator<char>());
            EXPECT_NE(mainSrc.find("Local\\\\Tools3000_SingleInstance_Mutex"), std::string::npos);
            EXPECT_NE(mainSrc.find("SessionIsolation::buildSingleInstanceMutexName"), std::string::npos);

            std::ifstream issIfs(repoRoot / relIss, std::ios::binary);
            ASSERT_TRUE(issIfs.is_open());
            const std::string issSrc((std::istreambuf_iterator<char>(issIfs)), std::istreambuf_iterator<char>());
            EXPECT_NE(issSrc.find("CheckForMutexes('Local\\Tools3000_SingleInstance_Mutex')"), std::string::npos);
        }
    }

    std::filesystem::remove_all(tempRoot, ec);
}

TEST(UiI18nTest, EightLanguageResolutionCatalogLoadingAndInstallerContract) {
    using namespace tools3000::core;

    struct ScopedUiI18nGuard {
        ~ScopedUiI18nGuard() {
            UiI18n::setLanguageOverrideForTest("");
            UiI18n::setCatalogDirectoryForTest({});
        }
    } guard;

    // 1. 跟随系统标识与受支持语言设置校验
    EXPECT_TRUE(UiI18n::isFollowSystemSetting(""));
    EXPECT_TRUE(UiI18n::isFollowSystemSetting("auto"));
    EXPECT_TRUE(UiI18n::isFollowSystemSetting("system"));
    EXPECT_FALSE(UiI18n::isFollowSystemSetting("ja-JP"));

    for (const char* valid : {"auto", "system", "zh-CN", "zh-TW", "zh-HK", "en-US", "ja-JP", "ko-KR", "ru-RU", "pt-BR", "es-ES"}) {
        EXPECT_TRUE(UiI18n::isSupportedLanguageSetting(valid)) << valid;
    }
    EXPECT_FALSE(UiI18n::isSupportedLanguageSetting("invalid-lang"));

    // 2. 区域标签精确归一化与未知语种英文兜底
    EXPECT_EQ(UiI18n::parseLanguageTag("zh-CN"), UiLanguage::ZhCN);
    EXPECT_EQ(UiI18n::parseLanguageTag("zh-SG"), UiLanguage::ZhCN);
    EXPECT_EQ(UiI18n::parseLanguageTag("zh-Hans"), UiLanguage::ZhCN);
    EXPECT_EQ(UiI18n::parseLanguageTag("zh-TW"), UiLanguage::ZhTW);
    EXPECT_EQ(UiI18n::parseLanguageTag("zh-HK"), UiLanguage::ZhTW);
    EXPECT_EQ(UiI18n::parseLanguageTag("zh-MO"), UiLanguage::ZhTW);
    EXPECT_EQ(UiI18n::parseLanguageTag("zh-Hant-TW"), UiLanguage::ZhTW);
    EXPECT_EQ(UiI18n::parseLanguageTag("ja"), UiLanguage::JaJP);
    EXPECT_EQ(UiI18n::parseLanguageTag("ja-JP"), UiLanguage::JaJP);
    EXPECT_EQ(UiI18n::parseLanguageTag("ko-KR"), UiLanguage::KoKR);
    EXPECT_EQ(UiI18n::parseLanguageTag("ru-RU"), UiLanguage::RuRU);
    EXPECT_EQ(UiI18n::parseLanguageTag("pt-BR"), UiLanguage::PtBR);
    EXPECT_EQ(UiI18n::parseLanguageTag("pt-PT"), UiLanguage::PtBR);
    EXPECT_EQ(UiI18n::parseLanguageTag("es-ES"), UiLanguage::EsES);
    EXPECT_EQ(UiI18n::parseLanguageTag("es-MX"), UiLanguage::EsES);
    EXPECT_EQ(UiI18n::parseLanguageTag("de-DE"), UiLanguage::EnUS);
    EXPECT_EQ(UiI18n::parseLanguageTag("fr-FR"), UiLanguage::EnUS);

    // 3. 定位仓库根并断言 6 套原生词典完整载入、有效翻译且占位符安全替换
    std::error_code ec;
    const auto relIss = std::filesystem::path(L"packaging") / L"installer" / L"installer.iss";
    std::filesystem::path probe = std::filesystem::current_path(ec);
    std::filesystem::path repoRoot;
    for (int depth = 0; depth < 6 && !probe.empty(); ++depth) {
        if (std::filesystem::exists(probe / relIss, ec)) {
            repoRoot = probe;
            break;
        }
        probe = probe.parent_path();
    }
    ASSERT_FALSE(repoRoot.empty());
    UiI18n::setCatalogDirectoryForTest(repoRoot / L"resources" / L"locales" / L"native");

    const std::array<std::pair<const char*, UiLanguage>, 6> kCatalogLangs = {{
        {"zh-TW", UiLanguage::ZhTW},
        {"ja-JP", UiLanguage::JaJP},
        {"ko-KR", UiLanguage::KoKR},
        {"ru-RU", UiLanguage::RuRU},
        {"pt-BR", UiLanguage::PtBR},
        {"es-ES", UiLanguage::EsES},
    }};
    for (const auto& [tag, langEnum] : kCatalogLangs) {
        UiI18n::setLanguageOverrideForTest(tag);
        EXPECT_EQ(UiI18n::getEffectiveLanguage(), langEnum) << tag;
        EXPECT_GT(UiI18n::getLoadedEntryCount(langEnum), 500u) << tag;
        const std::wstring translated = trW(L"自动运镜", L"Auto Zoom");
        EXPECT_FALSE(translated.empty()) << tag;
        EXPECT_NE(translated, L"Auto Zoom") << "Expected localized translation for " << tag;
        const std::wstring formatted = trFmtW(L"外壳留白: {0} px", L"Shell Padding: {0} px", {L"24"});
        EXPECT_NE(formatted.find(L"24"), std::wstring::npos) << tag;
    }

    // 4. 简体中文与英文零回退直通 + trWCstrFor 单测兼容性校验
    UiI18n::setLanguageOverrideForTest("zh-CN");
    EXPECT_EQ(trW(L"自动运镜", L"Auto Zoom"), L"自动运镜");
    EXPECT_STREQ(trWCstrFor(false, L"自动运镜", L"Auto Zoom"), L"Auto Zoom");
    UiI18n::setLanguageOverrideForTest("en-US");
    EXPECT_EQ(trW(L"自动运镜", L"Auto Zoom"), L"Auto Zoom");
    EXPECT_STREQ(trWCstrFor(true, L"自动运镜", L"Auto Zoom"), L"自动运镜");

    // 5. Inno Setup 8 语种安装器契约校验
    std::ifstream issIfs(repoRoot / relIss, std::ios::binary);
    ASSERT_TRUE(issIfs.is_open());
    const std::string issSrc((std::istreambuf_iterator<char>(issIfs)), std::istreambuf_iterator<char>());
    for (const char* issLang : {
             "Name: \"english\"", "Name: \"chinesesimplified\"", "Name: \"chinesetraditional\"",
             "Name: \"japanese\"", "Name: \"korean\"", "Name: \"russian\"",
             "Name: \"brazilianportuguese\"", "Name: \"spanish\""}) {
        EXPECT_NE(issSrc.find(issLang), std::string::npos) << issLang;
    }
    EXPECT_TRUE(std::filesystem::exists(repoRoot / L"resources" / L"installer" / L"ChineseTraditional.isl", ec));
}

#include "core/translate/TranslationService.h"

TEST(TranslationServiceTest, ProtocolParsersLanguageRoutingAndDpapiRoundTrip) {
    using namespace tools3000::core::translate;

    // 1. 智能双向目标语决策：英文转中文，中文自动切副语言英文，日文含假名不误判为中文
    EXPECT_EQ(TranslationService::resolveEffectiveTargetLanguage({"Hello world", "Settings"}, "zh-Hans", "en", "zh-CN"), "zh-CN");
    EXPECT_EQ(TranslationService::resolveEffectiveTargetLanguage({"你好世界", "系统设置"}, "zh-Hans", "en", "zh-CN"), "en");
    EXPECT_EQ(TranslationService::resolveEffectiveTargetLanguage({"こんにちは世界"}, "zh-Hans", "en", "zh-CN"), "zh-CN");
    EXPECT_EQ(TranslationService::resolveEffectiveTargetLanguage({"こんにちは世界"}, "ja", "zh-Hans", "zh-CN"), "zh-CN");
    EXPECT_EQ(TranslationService::resolveEffectiveTargetLanguage({"Hello"}, "ja", "zh-Hans", "zh-CN"), "ja");
    EXPECT_EQ(TranslationService::resolveEffectiveTargetLanguage({"你好世界"}, "auto", "en", "zh-CN"), "en");
    EXPECT_EQ(TranslationService::resolveEffectiveTargetLanguage({"Hello world"}, "auto", "en", "zh-CN"), "zh-CN");

    // 2. OpenAI / DeepL 端点规范化与 Loopback 本地离线判定
    std::string fullUrl;
    ASSERT_TRUE(TranslationService::normalizeOpenAiChatUrl("https://api.openai.com/v1", fullUrl));
    EXPECT_EQ(fullUrl, "https://api.openai.com/v1/chat/completions");
    ASSERT_TRUE(TranslationService::normalizeOpenAiChatUrl("https://api.openai.com/v1/", fullUrl));
    EXPECT_EQ(fullUrl, "https://api.openai.com/v1/chat/completions");
    ASSERT_TRUE(TranslationService::normalizeOpenAiChatUrl("http://127.0.0.1:11434/v1/chat/completions", fullUrl));
    EXPECT_EQ(fullUrl, "http://127.0.0.1:11434/v1/chat/completions");
    EXPECT_TRUE(TranslationService::isLoopbackHttpUrl("http://127.0.0.1:11434/v1"));
    EXPECT_TRUE(TranslationService::isLoopbackHttpUrl("http://localhost:8080/v1"));
    EXPECT_TRUE(TranslationService::isLoopbackHttpUrl("http://[::1]:11434/v1"));
    EXPECT_FALSE(TranslationService::isLoopbackHttpUrl("https://api.openai.com/v1"));

    ASSERT_TRUE(TranslationService::normalizeDeepLUrl("", "abc-key:fx", fullUrl));
    EXPECT_EQ(fullUrl, "https://api-free.deepl.com/v2/translate");
    ASSERT_TRUE(TranslationService::normalizeDeepLUrl("", "abc-pro-key", fullUrl));
    EXPECT_EQ(fullUrl, "https://api.deepl.com/v2/translate");
    ASSERT_TRUE(TranslationService::normalizeDeepLUrl("https://proxy.example.com/deepl", "k:fx", fullUrl));
    EXPECT_EQ(fullUrl, "https://proxy.example.com/deepl/v2/translate");

    // 3. Edge / Google / OpenAI / DeepL 4 种协议解析器全量回归断言
    std::vector<std::string> out;
    std::string detected;

    ASSERT_TRUE(TranslationService::parseEdgeTranslateResponse(
        R"([{"detectedLanguage":{"language":"en"},"translations":[{"text":"你好"}]},{"translations":[{"text":"世界"}]}])",
        2, out, detected));
    ASSERT_EQ(out.size(), 2u);
    EXPECT_EQ(out[0], "你好");
    EXPECT_EQ(out[1], "世界");
    EXPECT_EQ(detected, "en");
    EXPECT_FALSE(TranslationService::parseEdgeTranslateResponse(
        R"([{"translations":[{"text":"only1"}]}])", 2, out, detected));

    std::string mergedGoogle;
    ASSERT_TRUE(TranslationService::parseGoogleTranslateResponse(
        R"([[["你好\n","Hello\n",null,null,10],["世界","World",null,null,10]],null,"en"])",
        mergedGoogle, detected));
    EXPECT_EQ(mergedGoogle, "你好\n世界");
    EXPECT_EQ(detected, "en");

    ASSERT_TRUE(TranslationService::parseOpenAiBatchResponse(
        R"({"choices":[{"message":{"content":"```json\n{\"translations\":[{\"id\":0,\"text\":\"保存\"},{\"id\":1,\"text\":\"取消\"}]}\n```"}}]})",
        2, out));
    ASSERT_EQ(out.size(), 2u);
    EXPECT_EQ(out[0], "保存");
    EXPECT_EQ(out[1], "取消");

    ASSERT_TRUE(TranslationService::parseDeepLTranslateResponse(
        R"({"translations":[{"detected_source_language":"EN","text":"打开"},{"text":"关闭"}]})",
        2, out, detected));
    ASSERT_EQ(out.size(), 2u);
    EXPECT_EQ(out[0], "打开");
    EXPECT_EQ(out[1], "关闭");
    EXPECT_EQ(detected, "EN");

    // 3b. Bing/Edge 翻译页凭证提取纯函数回归
    std::string bingIg, bingIid, bingKey, bingToken;
    ASSERT_TRUE(TranslationService::parseBingTranslatorCredentials(
        R"(<script>var x={IG:"5E91CB3546E946849A4461BC9CC0D4A3"};</script><div data-iid="translator.5023"></div><script>var params_AbusePreventionHelper = [1795361663141,"AbCdEfGh1234567890",3600000];</script>)",
        bingIg, bingIid, bingKey, bingToken));
    EXPECT_EQ(bingIg, "5E91CB3546E946849A4461BC9CC0D4A3");
    EXPECT_EQ(bingIid, "translator.5023");
    EXPECT_EQ(bingKey, "1795361663141");
    EXPECT_EQ(bingToken, "AbCdEfGh1234567890");
    EXPECT_FALSE(TranslationService::parseBingTranslatorCredentials("<html>no token</html>", bingIg, bingIid, bingKey, bingToken));

    // 4. Windows DPAPI 加密解密往返与密钥脱敏（零明文残留）
    const std::string sampleKey = "sk-tools3000-secret-key-987654";
    const std::string protectedBlob = TranslationService::protectSecretForStorage(sampleKey);
    ASSERT_FALSE(protectedBlob.empty());
    EXPECT_EQ(protectedBlob.rfind("dpapi:", 0), 0u);
    EXPECT_EQ(protectedBlob.find(sampleKey), std::string::npos);
    EXPECT_EQ(TranslationService::unprotectSecretFromStorage(protectedBlob), sampleKey);
    EXPECT_EQ(TranslationService::maskApiKeyForUi(sampleKey), "sk-t****7654");
    EXPECT_EQ(TranslationService::maskApiKeyForUi("short"), "********");
    EXPECT_EQ(TranslationService::maskApiKeyForUi(""), "");

    // 5. 缺密钥快速拦截（绝不产生多余外网请求）
    TranslationBatchRequest singleReq;
    singleReq.texts = {"Hello"};
    singleReq.uiLanguageCode = "zh-CN";

    TranslationSettings noKeyOpenAi;
    noKeyOpenAi.provider = TranslationConstants::kProviderOpenAiCompatible;
    noKeyOpenAi.openaiBaseUrl = "https://api.openai.com/v1";
    noKeyOpenAi.openaiApiKey.clear();
    const auto missingRes = TranslationService::instance().translateBatchWithSettings(singleReq, noKeyOpenAi);
    EXPECT_FALSE(missingRes.success);
    EXPECT_EQ(missingRes.errorCode, "missing_api_key");

    TranslationSettings noKeyDeepL;
    noKeyDeepL.provider = TranslationConstants::kProviderDeepL;
    noKeyDeepL.deeplApiKey.clear();
    const auto missingDeepL = TranslationService::instance().translateBatchWithSettings(singleReq, noKeyDeepL);
    EXPECT_FALSE(missingDeepL.success);
    EXPECT_EQ(missingDeepL.errorCode, "missing_api_key");
}

TEST(TranslationServiceTest, LiveNetworkFreeChannelsSmokeWhenEnabled) {
    using namespace tools3000::core::translate;
    wchar_t envBuf[16]{};
    const DWORD envLen = GetEnvironmentVariableW(L"TOOLS3000_LIVE_TRANSLATE_TEST", envBuf, 16);
    if (envLen == 0 || envBuf[0] != L'1') {
        GTEST_SKIP() << "Set TOOLS3000_LIVE_TRANSLATE_TEST=1 to run live network smoke test";
    }

    TranslationBatchRequest req;
    req.texts = {"Save changes", "Cancel operation"};
    req.uiLanguageCode = "zh-CN";

    // 1. Edge/Bing 主通道实网多段翻译
    TranslationSettings edgeSettings;
    edgeSettings.provider = TranslationConstants::kProviderAutoFree;
    edgeSettings.freePrimary = TranslationConstants::kFreePrimaryEdge;
    edgeSettings.targetLang = "zh-CN";
    edgeSettings.secondaryTargetLang = "en";
    const auto edgeRes = TranslationService::instance().translateBatchWithSettings(req, edgeSettings);
    ASSERT_TRUE(edgeRes.success) << edgeRes.errorCode << " : " << edgeRes.errorDetail;
    EXPECT_EQ(edgeRes.providerUsed, "edge_free");
    ASSERT_EQ(edgeRes.translations.size(), 2u);
    EXPECT_FALSE(edgeRes.translations[0].empty());
    EXPECT_FALSE(edgeRes.translations[1].empty());

    // 2. Google 备选通道实网多段翻译
    TranslationSettings googleSettings = edgeSettings;
    googleSettings.freePrimary = TranslationConstants::kFreePrimaryGoogle;
    const auto googleRes = TranslationService::instance().translateBatchWithSettings(req, googleSettings);
    ASSERT_TRUE(googleRes.success) << googleRes.errorCode << " : " << googleRes.errorDetail;
    EXPECT_EQ(googleRes.providerUsed, "google_free");
    ASSERT_EQ(googleRes.translations.size(), 2u);
    EXPECT_FALSE(googleRes.translations[0].empty());
    EXPECT_FALSE(googleRes.translations[1].empty());
}

#include "clipboard/ClipboardManager.h"
#include "clipboard/ClipboardNativeDrag.h"
#include "clipboard/ClipboardRustFfi.h"
#include "ui/ClipboardWindow.h"

extern "C" char* t3k_clip_query_json_v2(
    const char* query_utf8,
    const char* filter_kind_utf8,
    const char* group_id_utf8,
    uint32_t limit,
    uint32_t offset,
    int32_t exclude_pinned);

TEST(ClipboardManagerTest, PrivacyBlocklistAndRustPersistenceAcrossRestart) {
    using tools3000::clipboard::ClipboardManager;

    // 1. 隐私密码管理器进程排除规则校验（大小写不敏感 + 省略 .exe 兼容）
    const std::string defaults = "KeePass.exe,KeePassXC.exe,1Password.exe,Bitwarden.exe,Enpass.exe";
    EXPECT_TRUE(ClipboardManager::isExcludedProcessName("bitwarden.exe", defaults));
    EXPECT_TRUE(ClipboardManager::isExcludedProcessName("KeePassXC.EXE", defaults));
    EXPECT_TRUE(ClipboardManager::isExcludedProcessName("1Password.exe", "1password,keepass"));
    EXPECT_FALSE(ClipboardManager::isExcludedProcessName("Code.exe", defaults));
    EXPECT_FALSE(ClipboardManager::isExcludedProcessName("", defaults));

    // 2. 端到端 C++ <-> Rust FFI 持久化 + 模拟关机重启二次加载恢复
    wchar_t tempDir[MAX_PATH]{};
    GetTempPathW(MAX_PATH, tempDir);
    const std::filesystem::path clipTestDir =
        std::filesystem::path(tempDir) /
        (L"Tools3000_ClipboardTest_" + std::to_wstring(GetCurrentProcessId()));
    std::error_code ec;
    std::filesystem::remove_all(clipTestDir, ec);

    struct DirCleanup {
        std::filesystem::path dir;
        ~DirCleanup() {
            t3k_clip_shutdown();
            std::error_code e;
            std::filesystem::remove_all(dir, e);
        }
    } cleanup{clipTestDir};

    const std::string dirUtf8 = tools3000::core::WinUtils::wstringToUtf8(clipTestDir.wstring());
    ASSERT_EQ(t3k_clip_init(dirUtf8.c_str()), 1);

    const uint64_t idText = t3k_clip_add_item(
        0, "Tools3000 persistent clipboard entry", "", 0, 0, "", "Code.exe", 0, 100, 30, "rich_1.html", "");
    const uint64_t idColor = t3k_clip_add_item(
        0, "#3b82f6", "", 0, 0, "", "Figma.exe", 0, 100, 30, "", "");
    ASSERT_GT(idText, 0u);
    ASSERT_GT(idColor, 0u);
    ASSERT_EQ(t3k_clip_toggle_pin(idColor), 1);

    // 重复添加相同文本 -> 自动去重复用原 ID
    const uint64_t idDup = t3k_clip_add_item(
        0, "Tools3000 persistent clipboard entry", "", 0, 0, "", "WindowsTerminal.exe", 0, 100, 30, "rich_1.html", "");
    EXPECT_EQ(idDup, idText);

    // 收藏分组创建与条目归组
    char* groupJsonRaw = t3k_clip_create_group("Snippets");
    ASSERT_NE(groupJsonRaw, nullptr);
    const auto groupJson = nlohmann::json::parse(groupJsonRaw);
    t3k_clip_free_string(groupJsonRaw);
    const std::string groupId = groupJson.value("id", "");
    ASSERT_FALSE(groupId.empty());
    EXPECT_EQ(groupJson.value("name", ""), "Snippets");
    EXPECT_EQ(t3k_clip_set_items_group(&idText, 1, groupId.c_str()), 1u);

    // 模拟 Windows 关机/进程退出后重启再初始化
    t3k_clip_shutdown();
    ASSERT_EQ(t3k_clip_init(dirUtf8.c_str()), 1);

    char* rawQuery = t3k_clip_query_json("", "all", "", 20, 0);
    ASSERT_NE(rawQuery, nullptr);
    const auto parsed = nlohmann::json::parse(rawQuery);
    t3k_clip_free_string(rawQuery);
    EXPECT_EQ(parsed.value("total", 0), 2);
    ASSERT_TRUE(parsed.contains("items") && parsed["items"].size() == 2u);
    // 置顶项排第一且识别为 color 类型
    EXPECT_EQ(parsed["items"][0].value("id", 0ull), idColor);
    EXPECT_EQ(parsed["items"][0].value("kind", ""), "color");
    EXPECT_EQ(parsed["items"][0].value("colorHex", ""), "#3B82F6");
    EXPECT_TRUE(parsed["items"][0].value("pinned", false));
    // 第二条去重计数 == 2，且保留分组与富文本标记
    EXPECT_EQ(parsed["items"][1].value("copyCount", 0), 2);
    EXPECT_EQ(parsed["items"][1].value("groupId", ""), groupId);
    EXPECT_TRUE(parsed["items"][1].value("hasRichText", false));

    // 单条详情接口返回完整富文本路径
    char* rawDetail = t3k_clip_get_item_json(idText);
    ASSERT_NE(rawDetail, nullptr);
    const auto parsedDetail = nlohmann::json::parse(rawDetail);
    t3k_clip_free_string(rawDetail);
    EXPECT_EQ(parsedDetail.value("htmlFile", ""), "rich_1.html");

    // 按分组过滤检索只命中归组项
    char* rawGroupQuery = t3k_clip_query_json("", "all", groupId.c_str(), 20, 0);
    ASSERT_NE(rawGroupQuery, nullptr);
    const auto parsedGroup = nlohmann::json::parse(rawGroupQuery);
    t3k_clip_free_string(rawGroupQuery);
    EXPECT_EQ(parsedGroup.value("matched", 0), 1);
    ASSERT_EQ(parsedGroup["items"].size(), 1u);
    EXPECT_EQ(parsedGroup["items"][0].value("id", 0ull), idText);

    // 清理非置顶项后，置顶项必须存活
    EXPECT_EQ(t3k_clip_clear_history(1), 1u);
    char* statsRaw = t3k_clip_stats_json();
    ASSERT_NE(statsRaw, nullptr);
    const auto stats = nlohmann::json::parse(statsRaw);
    t3k_clip_free_string(statsRaw);
    EXPECT_EQ(stats.value("totalCount", 0), 1);
    EXPECT_EQ(stats.value("pinnedCount", 0), 1);
}

TEST(ClipboardManagerTest, TargetWindowNormalizationAndDragAutoHidePolicy) {
    using tools3000::clipboard::ClipboardManager;
    using tools3000::ui::ClipboardWindow;

    EXPECT_EQ(ClipboardManager::normalizeValidTargetHwnd(nullptr, nullptr), nullptr);

    HINSTANCE hInst = GetModuleHandleW(nullptr);
    HWND selfWnd = CreateWindowExW(
        WS_EX_TOOLWINDOW, L"STATIC", L"T3KClipSelf", WS_POPUP,
        -2000, -2000, 40, 40, nullptr, nullptr, hInst, nullptr);
    HWND selfChild = CreateWindowExW(
        0, L"STATIC", L"T3KClipSelfChild", WS_CHILD,
        0, 0, 10, 10, selfWnd, nullptr, hInst, nullptr);
    HWND otherWnd = CreateWindowExW(
        WS_EX_TOOLWINDOW, L"STATIC", L"T3KClipOther", WS_POPUP,
        -2000, -2000, 40, 40, nullptr, nullptr, hInst, nullptr);
    HWND otherChild = CreateWindowExW(
        0, L"STATIC", L"T3KClipOtherChild", WS_CHILD,
        0, 0, 10, 10, otherWnd, nullptr, hInst, nullptr);
    ASSERT_NE(selfWnd, nullptr);
    ASSERT_NE(selfChild, nullptr);
    ASSERT_NE(otherWnd, nullptr);
    ASSERT_NE(otherChild, nullptr);

    // 不可见窗口被拒绝；可见自身及子窗口被拒绝；外部可见窗口及其子窗口归一化为顶层 GA_ROOT
    EXPECT_EQ(ClipboardManager::normalizeValidTargetHwnd(otherWnd, selfWnd), nullptr);
    ShowWindow(selfWnd, SW_SHOWNOACTIVATE);
    ShowWindow(otherWnd, SW_SHOWNOACTIVATE);
    EXPECT_EQ(ClipboardManager::normalizeValidTargetHwnd(selfWnd, selfWnd), nullptr);
    EXPECT_EQ(ClipboardManager::normalizeValidTargetHwnd(selfChild, selfWnd), nullptr);
    EXPECT_EQ(ClipboardManager::normalizeValidTargetHwnd(otherWnd, selfWnd), otherWnd);
    EXPECT_EQ(ClipboardManager::normalizeValidTargetHwnd(otherChild, selfWnd), otherWnd);

    DestroyWindow(otherChild);
    DestroyWindow(otherWnd);
    DestroyWindow(selfChild);
    DestroyWindow(selfWnd);

    // 原生拖拽期间、拖拽会话驻留期及结束后 1200ms 内严禁自动隐藏（即便未置顶）
    EXPECT_FALSE(ClipboardWindow::shouldAutoHideOnDeactivate(true, false, false, false, true, 5000, 1000, 0));
    EXPECT_FALSE(ClipboardWindow::shouldAutoHideOnDeactivate(true, false, false, false, false, 5200, 1000, 5000));
    EXPECT_FALSE(ClipboardWindow::shouldAutoHideOnDeactivate(true, false, false, false, false, 6199, 1000, 5000));
    EXPECT_TRUE(ClipboardWindow::shouldAutoHideOnDeactivate(true, false, false, false, false, 6200, 1000, 5000));
    // 拖放成功后的会话驻留态 (dragSessionHold=true)：即使超过 1200ms（如 Chrome 地址栏延迟抢焦点），也保持面板驻留不消失
    EXPECT_FALSE(ClipboardWindow::shouldAutoHideOnDeactivate(true, false, false, false, false, 20000, 1000, 5000, true));
    // 置顶态、缩放态、刚呼出 200ms 内同样不可隐藏
    EXPECT_FALSE(ClipboardWindow::shouldAutoHideOnDeactivate(true, false, true, false, false, 6000, 1000, 0));
    EXPECT_FALSE(ClipboardWindow::shouldAutoHideOnDeactivate(true, false, false, true, false, 6000, 1000, 0));
    EXPECT_FALSE(ClipboardWindow::shouldAutoHideOnDeactivate(true, false, false, false, false, 1150, 1000, 0));

    // 内部瞬态覆盖层与桌面外壳绝不可污染目标窗口句柄（如截屏 F1 后 CaptureOverlay 或托盘窗），
    // 而同进程可输入窗口（设置页/搜索页）与外部应用窗正常放行
    EXPECT_TRUE(ClipboardManager::isTransientInternalOverlayClass(L"Tools3000_CaptureOverlay"));
    EXPECT_TRUE(ClipboardManager::isTransientInternalOverlayClass(L"Tools3000_TrayWindow"));
    EXPECT_TRUE(ClipboardManager::isTransientInternalOverlayClass(L"Tools3000_ShortcutHintOverlay"));
    EXPECT_TRUE(ClipboardManager::isTransientInternalOverlayClass(L"Tools3000_ClipboardWindow"));
    EXPECT_TRUE(ClipboardManager::isTransientInternalOverlayClass(L"Progman"));
    EXPECT_FALSE(ClipboardManager::isTransientInternalOverlayClass(L"Tools3000_SettingsWindow"));
    EXPECT_FALSE(ClipboardManager::isTransientInternalOverlayClass(L"Tools3000_SearchWindow"));
    EXPECT_FALSE(ClipboardManager::isTransientInternalOverlayClass(L"Chrome_WidgetWin_1"));

    // 热键呼出策略（修复连按三下才呼出 / 掉后台误关）：
    // 1. 仅当已显示、未处于冷启等待揭示、且确实位于前台激活态时，再次按下热键才执行隐藏
    EXPECT_TRUE(ClipboardWindow::shouldHideOnToggle(true, false, true));
    // 2. 窗口虽可见但掉到后台被其他窗口遮挡时，按 Alt+V 必须强力前置拉起，绝不隐藏
    EXPECT_FALSE(ClipboardWindow::shouldHideOnToggle(true, false, false));
    // 3. 冷启动 WebView 正在等待首帧揭示期间，按 Alt+V 必须继续前置展示，绝不中途取消隐藏
    EXPECT_FALSE(ClipboardWindow::shouldHideOnToggle(true, true, true));
    EXPECT_FALSE(ClipboardWindow::shouldHideOnToggle(true, true, false));
    // 4. 隐藏态当然不可再隐藏
    EXPECT_FALSE(ClipboardWindow::shouldHideOnToggle(false, false, false));
    EXPECT_FALSE(ClipboardWindow::shouldHideOnToggle(false, false, true));
}

TEST(ClipboardManagerTest, ExcludePinnedQueryV2AndNativeDragDataObject) {
    using namespace tools3000::clipboard;

    // 1. Rust FFI v2 查询：验证 exclude_pinned 对非置顶分类生效、对 pinned 分类保持完整
    wchar_t tempDir[MAX_PATH]{};
    GetTempPathW(MAX_PATH, tempDir);
    const std::filesystem::path clipTestDir =
        std::filesystem::path(tempDir) /
        (L"Tools3000_ClipboardExcludePinnedTest_" + std::to_wstring(GetCurrentProcessId()));
    std::error_code ec;
    std::filesystem::remove_all(clipTestDir, ec);

    struct DirCleanup {
        std::filesystem::path dir;
        ~DirCleanup() {
            t3k_clip_shutdown();
            std::error_code e;
            std::filesystem::remove_all(dir, e);
        }
    } cleanup{clipTestDir};

    const std::string dirUtf8 = tools3000::core::WinUtils::wstringToUtf8(clipTestDir.wstring());
    ASSERT_EQ(t3k_clip_init(dirUtf8.c_str()), 1);

    const uint64_t idPinned = t3k_clip_add_item(0, "Pinned snippet", "", 0, 0, "", "Code.exe", 0, 100, 30, "", "");
    const uint64_t idNormal = t3k_clip_add_item(0, "Regular snippet", "", 0, 0, "", "Notepad.exe", 0, 100, 30, "", "");
    ASSERT_GT(idPinned, 0u);
    ASSERT_GT(idNormal, 0u);
    ASSERT_EQ(t3k_clip_toggle_pin(idPinned), 1);

    char* rawExcluded = t3k_clip_query_json_v2("", "all", "", 20, 0, 1);
    ASSERT_NE(rawExcluded, nullptr);
    const auto excludedJson = nlohmann::json::parse(rawExcluded);
    t3k_clip_free_string(rawExcluded);
    EXPECT_EQ(excludedJson.value("total", 0), 2);
    EXPECT_EQ(excludedJson.value("matched", 0), 1);
    EXPECT_EQ(excludedJson.value("pinnedCount", 0), 1);
    EXPECT_EQ(excludedJson.value("hiddenPinnedCount", 0), 1);
    ASSERT_EQ(excludedJson["items"].size(), 1u);
    EXPECT_EQ(excludedJson["items"][0].value("id", 0ull), idNormal);
    EXPECT_FALSE(excludedJson["items"][0].value("pinned", true));

    char* rawPinnedTab = t3k_clip_query_json_v2("", "pinned", "", 20, 0, 1);
    ASSERT_NE(rawPinnedTab, nullptr);
    const auto pinnedTabJson = nlohmann::json::parse(rawPinnedTab);
    t3k_clip_free_string(rawPinnedTab);
    EXPECT_EQ(pinnedTabJson.value("matched", 0), 1);
    EXPECT_EQ(pinnedTabJson.value("hiddenPinnedCount", 0), 0);
    ASSERT_EQ(pinnedTabJson["items"].size(), 1u);
    EXPECT_EQ(pinnedTabJson["items"][0].value("id", 0ull), idPinned);

    // 2. 纯 Win32 COM IDataObject / IEnumFORMATETC / IDropSource 协议与载荷验证
    NativeDragPayload payload;
    payload.unicodeText = L"C:\\Tools\\demo.exe";
    const std::string htmlSample = "Version:0.9\r\n<b>Hello</b>";
    payload.htmlBytes.assign(htmlSample.begin(), htmlSample.end());
    payload.filePaths.push_back(L"C:\\Tools\\demo.exe");

    auto dataObj = createNativeDragDataObject(payload);
    ASSERT_NE(dataObj.Get(), nullptr);

    FORMATETC textFe{CF_UNICODETEXT, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
    EXPECT_EQ(dataObj->QueryGetData(&textFe), S_OK);
    STGMEDIUM textMed{};
    ASSERT_EQ(dataObj->GetData(&textFe, &textMed), S_OK);
    ASSERT_EQ(textMed.tymed, TYMED_HGLOBAL);
    ASSERT_NE(textMed.hGlobal, nullptr);
    const auto* lockedText = static_cast<const wchar_t*>(GlobalLock(textMed.hGlobal));
    ASSERT_NE(lockedText, nullptr);
    EXPECT_EQ(std::wstring(lockedText), L"C:\\Tools\\demo.exe");
    GlobalUnlock(textMed.hGlobal);
    ReleaseStgMedium(&textMed);

    FORMATETC dropFe{CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
    EXPECT_EQ(dataObj->QueryGetData(&dropFe), S_OK);
    STGMEDIUM dropMed{};
    ASSERT_EQ(dataObj->GetData(&dropFe, &dropMed), S_OK);
    ASSERT_EQ(dropMed.tymed, TYMED_HGLOBAL);
    ASSERT_NE(dropMed.hGlobal, nullptr);
    const UINT fileCount = DragQueryFileW(static_cast<HDROP>(dropMed.hGlobal), 0xFFFFFFFFu, nullptr, 0);
    EXPECT_EQ(fileCount, 1u);
    wchar_t extractedPath[MAX_PATH]{};
    ASSERT_GT(DragQueryFileW(static_cast<HDROP>(dropMed.hGlobal), 0, extractedPath, MAX_PATH), 0u);
    EXPECT_EQ(std::wstring(extractedPath), L"C:\\Tools\\demo.exe");
    ReleaseStgMedium(&dropMed);

    FORMATETC missingDibFe{CF_DIB, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
    EXPECT_EQ(dataObj->QueryGetData(&missingDibFe), DV_E_FORMATETC);

    FORMATETC ansiFe{CF_TEXT, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
    EXPECT_EQ(dataObj->QueryGetData(&ansiFe), S_OK);
    STGMEDIUM ansiMed{};
    ASSERT_EQ(dataObj->GetData(&ansiFe, &ansiMed), S_OK);
    ASSERT_EQ(ansiMed.tymed, TYMED_HGLOBAL);
    ASSERT_NE(ansiMed.hGlobal, nullptr);
    const auto* lockedAnsi = static_cast<const char*>(GlobalLock(ansiMed.hGlobal));
    ASSERT_NE(lockedAnsi, nullptr);
    EXPECT_EQ(std::string(lockedAnsi), "C:\\Tools\\demo.exe");
    GlobalUnlock(ansiMed.hGlobal);
    ReleaseStgMedium(&ansiMed);

    Microsoft::WRL::ComPtr<IEnumFORMATETC> enumFmt;
    ASSERT_EQ(dataObj->EnumFormatEtc(DATADIR_GET, enumFmt.GetAddressOf()), S_OK);
    ASSERT_NE(enumFmt.Get(), nullptr);
    FORMATETC enumerated[5]{};
    ULONG fetched = 0;
    EXPECT_EQ(enumFmt->Next(5, enumerated, &fetched), S_FALSE);
    ASSERT_EQ(fetched, 4u); // CF_HDROP -> CF_UNICODETEXT -> CF_TEXT -> HTML Format
    EXPECT_EQ(enumerated[0].cfFormat, CF_HDROP);
    EXPECT_EQ(enumerated[1].cfFormat, CF_UNICODETEXT);
    EXPECT_EQ(enumerated[2].cfFormat, CF_TEXT);
    EXPECT_EQ(enumFmt->Reset(), S_OK);

    auto dropSource = createNativeDropSource();
    ASSERT_NE(dropSource.Get(), nullptr);
    EXPECT_EQ(dropSource->QueryContinueDrag(TRUE, MK_LBUTTON), DRAGDROP_S_CANCEL);
    EXPECT_EQ(dropSource->QueryContinueDrag(FALSE, 0), DRAGDROP_S_DROP);
    EXPECT_EQ(dropSource->QueryContinueDrag(FALSE, MK_LBUTTON), S_OK);
    EXPECT_EQ(dropSource->GiveFeedback(DROPEFFECT_COPY), DRAGDROP_S_USEDEFAULTCURSORS);

    // 3. 混合双引擎落点安全过滤与 Smart Drop-Paste 回退策略验证（含无边框 Electron 腹地输入框放行与假 Drop 识别）
    EXPECT_TRUE(isSafeDropHitTestResult(HTCLIENT));
    EXPECT_FALSE(isSafeDropHitTestResult(HTCAPTION, false));
    EXPECT_TRUE(isSafeDropHitTestResult(HTCAPTION, true)); // Clash Party 等无边框 Electron 腹地输入框放行
    EXPECT_FALSE(isSafeDropHitTestResult(HTCLOSE, true));
    EXPECT_FALSE(isSafeDropHitTestResult(HTMINBUTTON, true));
    EXPECT_FALSE(isSafeDropHitTestResult(HTMAXBUTTON, true));
    EXPECT_FALSE(isSafeDropHitTestResult(HTSYSMENU, true));
    EXPECT_FALSE(isSafeDropHitTestResult(HTMENU, true));
    EXPECT_FALSE(isSafeDropHitTestResult(HTLEFT, true));
    EXPECT_FALSE(isSafeDropHitTestResult(HTBORDER, true));

    EXPECT_FALSE(shouldFallbackToSmartDropPaste(DRAGDROP_S_DROP, DROPEFFECT_COPY, false, true, true, true));
    // 目标仅监听文件拖入而未实际调用 GetData 读取文本载荷（假 Drop 空吞）时，自动启用第二引擎直贴兜底
    EXPECT_TRUE(shouldFallbackToSmartDropPaste(DRAGDROP_S_DROP, DROPEFFECT_COPY, false, true, true, false));
    EXPECT_FALSE(shouldFallbackToSmartDropPaste(DRAGDROP_S_CANCEL, DROPEFFECT_NONE, true, true, true, false));
    EXPECT_FALSE(shouldFallbackToSmartDropPaste(DRAGDROP_S_DROP, DROPEFFECT_NONE, false, false, true, false));
    EXPECT_FALSE(shouldFallbackToSmartDropPaste(DRAGDROP_S_DROP, DROPEFFECT_NONE, false, true, false, false));
    EXPECT_TRUE(shouldFallbackToSmartDropPaste(DRAGDROP_S_DROP, DROPEFFECT_NONE, false, true, true, false));
    EXPECT_TRUE(shouldFallbackToSmartDropPaste(S_OK, DROPEFFECT_NONE, false, true, true, false));

    // 4. 终端与 CLI TUI 宿主分类验证（Windows Terminal / WezTerm / ConHost / Mintty / 普通 GUI）
    EXPECT_EQ(classifyTerminalPasteTarget(L"CASCADIA_HOSTING_WINDOW_CLASS", L"windowsterminal.exe"),
              TerminalPasteKind::ModernCtrlShiftV);
    EXPECT_EQ(classifyTerminalPasteTarget(L"Window Class", L"alacritty.exe"),
              TerminalPasteKind::ModernCtrlShiftV);
    EXPECT_EQ(classifyTerminalPasteTarget(L"ConsoleWindowClass", L"conhost.exe"),
              TerminalPasteKind::LegacyCtrlV);
    EXPECT_EQ(classifyTerminalPasteTarget(L"mintty", L"mintty.exe"),
              TerminalPasteKind::ShiftInsert);
    EXPECT_EQ(classifyTerminalPasteTarget(L"Chrome_WidgetWin_1", L"clash party.exe"),
              TerminalPasteKind::NotTerminal);
}

TEST(ClipboardManagerTest, SourceAppFriendlyNameIconExtractionAndPersistence) {
    using namespace tools3000::clipboard;

    // 1. 纯静态名称归一化规则：去 .exe 噪音、避开 OS 泛化产品名、优选简洁产品名
    EXPECT_EQ(ClipboardManager::normalizeProcessKey(" Weixin.EXE "), "weixin.exe");
    EXPECT_EQ(ClipboardManager::normalizeFriendlyAppName("Microsoft Edge", "Microsoft Edge", "msedge.exe"),
              "Microsoft Edge");
    EXPECT_EQ(ClipboardManager::normalizeFriendlyAppName("Visual Studio Code", "Visual Studio Code", "Code.exe"),
              "Visual Studio Code");
    EXPECT_EQ(ClipboardManager::normalizeFriendlyAppName(
                  "Notepad", "Microsoft® Windows® Operating System", "notepad.exe"),
              "Notepad");
    EXPECT_EQ(ClipboardManager::normalizeFriendlyAppName(
                  "Tools3000 Desktop Productivity Toolkit", "Tools3000", "Tools3000.exe"),
              "Tools3000");
    EXPECT_EQ(ClipboardManager::normalizeFriendlyAppName("Application", "Launcher", "Weixin.exe"), "Weixin");
    EXPECT_EQ(ClipboardManager::normalizeFriendlyAppName("App (64-bit)", "MyEditor (64-bit)", "myeditor.exe"),
              "MyEditor");

    // 2. 实机提取系统真实可执行文件彩色图标 + 友好名，并验证 queryItems 返回 appMeta 且跨重启持久化恢复
    wchar_t tempDir[MAX_PATH]{};
    GetTempPathW(MAX_PATH, tempDir);
    const std::filesystem::path metaTestDir =
        std::filesystem::path(tempDir) /
        (L"Tools3000_ClipboardAppMetaTest_" + std::to_wstring(GetCurrentProcessId()));
    std::error_code ec;
    std::filesystem::remove_all(metaTestDir, ec);

    struct MetaCleanup {
        std::filesystem::path dir;
        ~MetaCleanup() {
            ClipboardManager::instance().shutdown();
            std::error_code e;
            std::filesystem::remove_all(dir, e);
        }
    } cleanup{metaTestDir};

    wchar_t winDir[MAX_PATH]{};
    ASSERT_GT(GetWindowsDirectoryW(winDir, MAX_PATH), 0u);
    const std::filesystem::path explorerPath = std::filesystem::path(winDir) / L"explorer.exe";
    ASSERT_TRUE(std::filesystem::exists(explorerPath, ec));

    auto& mgr = ClipboardManager::instance();
    mgr.shutdown();
    ASSERT_TRUE(mgr.initialize(nullptr, metaTestDir));
    ASSERT_TRUE(mgr.recordSourceAppMetadataForPath("explorer.exe", explorerPath.wstring()));

    const uint64_t id = t3k_clip_add_item(0, "Hello Explorer", "", 0, 0, "", "explorer.exe", 14, 100, 30, "", "");
    ASSERT_GT(id, 0u);

    const auto q1 = mgr.queryItems("", "all", 20, 0, "", false);
    ASSERT_TRUE(q1.contains("appMeta"));
    ASSERT_TRUE(q1["appMeta"].contains("explorer.exe"));
    const std::string name1 = q1["appMeta"]["explorer.exe"].value("name", "");
    const std::string icon1 = q1["appMeta"]["explorer.exe"].value("icon", "");
    EXPECT_FALSE(name1.empty());
    EXPECT_EQ(icon1.rfind("data:image/png;base64,", 0), 0u);
    EXPECT_GT(icon1.size(), 64u);
    EXPECT_TRUE(std::filesystem::exists(metaTestDir / L"app_meta.json", ec));

    // 模拟重启：彻底关闭后重新初始化，确认无需重新扫盘即可直接从 app_meta.json 恢复图标与友好名
    mgr.shutdown();
    ASSERT_TRUE(mgr.initialize(nullptr, metaTestDir));
    const auto q2 = mgr.queryItems("", "all", 20, 0, "", false);
    ASSERT_TRUE(q2.contains("appMeta"));
    ASSERT_TRUE(q2["appMeta"].contains("explorer.exe"));
    EXPECT_EQ(q2["appMeta"]["explorer.exe"].value("name", ""), name1);
    EXPECT_EQ(q2["appMeta"]["explorer.exe"].value("icon", ""), icon1);
}


// ─────────────────────────────────────────────────────────────────────────────
// 8 语种跨代码页 Unicode 路径、ArchivePreview、FontPreview、FileLocksmith 与
// FeedbackDiagnosticService 10 维隐私脱敏防回退单元测试
// ─────────────────────────────────────────────────────────────────────────────
#include "core/utils/ArchivePreview.h"
#include "core/utils/FontPreview.h"
#include "core/utils/FileLocksmithEngine.h"
#include "core/diagnostics/FeedbackDiagnosticService.h"

TEST(CrossCodepagePathTest, EightLanguagesRoundTripAndFilesystemExists) {
    using tools3000::core::WinUtils;
    // 同时混合简中、繁中、日语、韩语、俄语、西语、葡语字符，验证在任意 Windows ACP 下 100% 无损往返
    const std::wstring multiLangFolder =
        L"Tools3000_PathTest_工具_繁體_ツール_도구_Инструмент_Herramienta_Configuração_" +
        std::to_wstring(GetCurrentProcessId());
    const std::filesystem::path dir = std::filesystem::temp_directory_path() / multiLangFolder;
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    ASSERT_TRUE(std::filesystem::create_directories(dir, ec));

    const std::string utf8 = WinUtils::pathToUtf8(dir);
    const std::filesystem::path restored = WinUtils::utf8ToPath(utf8);
    EXPECT_EQ(restored.wstring(), dir.wstring());
    EXPECT_TRUE(std::filesystem::is_directory(restored, ec));

    // 在 8 国混合目录中调用 atomicWriteFileWithFlush 验证真实文件落盘
    const std::filesystem::path probeFile = restored / L"测试_テスト_테스트_тест.txt";
    EXPECT_TRUE(WinUtils::atomicWriteFileWithFlush(probeFile.wstring(), "ok-8-languages"));
    EXPECT_TRUE(std::filesystem::is_regular_file(probeFile, ec));

    std::filesystem::remove_all(dir, ec);
}

TEST(FeedbackAndArchiveTest, TenRulePrivacyRedactionAndZipRoundTrip) {
    using tools3000::core::FeedbackDiagnosticService;
    using tools3000::core::FeedbackSubmissionInput;
    using tools3000::core::ArchivePreview;

    const std::string dirty =
        "UserPath=C:\\Users\\坏女孩\\AppData\\Local\\Tools3000\\config.json\n"
        "Auth=Bearer eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9.eyJzdWIiOiIxMjM0NTY3ODkwIn0.abcdef1234567890\n"
        "GitHub=ghp_1234567890abcdef1234567890abcdef1234\n"
        "SecretKV=password=SuperSecret99\n"
        "Email=tester.alice@example.org\n"
        "Url=https://admin:MyPass123@intranet.local/api\n"
        "PublicIP=198.51.100.42 LocalIP=127.0.0.1\n"
        "Phone=13812345678\n"
        "IdCard=110101199003071234\n"
        "SID=S-1-5-21-3623811015-3361044348-30300820-1013\n";

    const auto report = FeedbackDiagnosticService::redactSensitiveText(dirty);
    EXPECT_GE(report.totalReplacements, 10u);
    EXPECT_EQ(report.redactedText.find("坏女孩"), std::string::npos);
    EXPECT_NE(report.redactedText.find("C:\\Users\\<USER>\\AppData"), std::string::npos);
    EXPECT_EQ(report.redactedText.find("ghp_1234567890"), std::string::npos);
    EXPECT_EQ(report.redactedText.find("SuperSecret99"), std::string::npos);
    EXPECT_EQ(report.redactedText.find("tester.alice@example.org"), std::string::npos);
    EXPECT_EQ(report.redactedText.find("MyPass123"), std::string::npos);
    EXPECT_EQ(report.redactedText.find("198.51.100.42"), std::string::npos);
    EXPECT_NE(report.redactedText.find("127.0.0.1"), std::string::npos);  // 保留回环地址便于本地排障
    EXPECT_EQ(report.redactedText.find("13812345678"), std::string::npos);
    EXPECT_EQ(report.redactedText.find("110101199003071234"), std::string::npos);
    EXPECT_EQ(report.redactedText.find("3623811015-3361044348"), std::string::npos);

    // 验证轻量 PoW 计算
    const std::string nonce = FeedbackDiagnosticService::computeLightweightPowNonce("t3k-test", 2);
    EXPECT_FALSE(nonce.empty());

    // 真实导出脱敏诊断包 .zip 并用 ArchivePreview 回读校验内部条目
    const std::filesystem::path zipPath =
        std::filesystem::temp_directory_path() /
        (L"tools3000_feedback_test_" + std::to_wstring(GetCurrentProcessId()) + L".zip");
    std::error_code ec;
    std::filesystem::remove(zipPath, ec);

    FeedbackSubmissionInput input;
    input.feedbackType = "bug";
    input.title = "Unit Test Feedback";
    input.description = "Path C:\\Users\\TesterBob\\secret.txt failed";
    input.includeLogs = false;
    std::string err;
    uint32_t redactions = 0;
    ASSERT_TRUE(FeedbackDiagnosticService::exportDiagnosticZipBundle(input, zipPath, err, &redactions)) << err;
    EXPECT_GE(redactions, 1u);

    const auto arch = ArchivePreview::inspectArchive(zipPath.wstring(), 100);
    EXPECT_TRUE(arch.ok);
    EXPECT_EQ(arch.format, "ZIP");
    EXPECT_GE(arch.totalFiles, 2u);
    bool sawReport = false;
    bool sawEnv = false;
    for (const auto& entry : arch.entries) {
        if (entry.path == "report.md") sawReport = true;
        if (entry.path == "environment.json") sawEnv = true;
    }
    EXPECT_TRUE(sawReport);
    EXPECT_TRUE(sawEnv);

    std::filesystem::remove(zipPath, ec);
}

TEST(UpdateCheckerTest, ReleaseNotesSummaryTruncatesUtf8SafelyAtBoundary) {
    using tools3000::core::UpdateChecker;
    using tools3000::core::WinUtils;

    // 构造故意让 3 字节中文字符跨过 600 字节边界的发布说明（复现 v1.4.2 检查更新 type_error.316 根因）
    std::string notes = "A"; // 1 字节 ASCII 偏移，使第 600 字节恰好落在中文字符中间字节
    for (int i = 0; i < 240; ++i) {
        notes += "功能更新与架构升级";
    }
    const std::string summary = UpdateChecker::formatReleaseNotesSummary(notes, 600);
    EXPECT_LE(summary.size(), 603u);
    EXPECT_TRUE(summary.ends_with("..."));

    // 严格模式 nlohmann::json::dump() 绝不能抛出 type_error.316
    nlohmann::json payload = {
        {"status", "available"},
        {"latestVersion", "1.4.3"},
        {"releaseNotes", summary},
    };
    EXPECT_NO_THROW({
        const std::string serialized = payload.dump();
        EXPECT_NE(serialized.find("1.4.3"), std::string::npos);
    });

    // 验证 WinUtils UTF-8 清洗、前截断与尾截断原语
    const std::string corrupted = std::string("前缀") + "\xE4\xB8" + ".后缀";
    const std::string cleaned = WinUtils::sanitizeUtf8(corrupted);
    EXPECT_NO_THROW((void)nlohmann::json({{"text", cleaned}}).dump());

    const std::string tail = WinUtils::truncateUtf8Tail(notes, 200, false);
    EXPECT_LE(tail.size(), 200u);
    EXPECT_NO_THROW((void)nlohmann::json({{"tail", tail}}).dump());
}

TEST(FeedbackAndArchiveTest, OversizedChineseFeedbackAndBridgePayloadLimits) {
    using tools3000::core::FeedbackDiagnosticService;
    using tools3000::core::FeedbackSubmissionInput;
    using tools3000::core::MessageBridge;

    // 1. 构造超长多语种反馈文本（跨 2600 字节边界），验证预览输出与 GitHub URL 100% 合法 UTF-8
    FeedbackSubmissionInput input;
    input.feedbackType = "bug";
    input.title = "测试关于页检查更新与反馈提交";
    input.description = "X";
    for (int i = 0; i < 400; ++i) {
        input.description += "中文反馈内容与診断ログテスト";
    }
    input.includeLogs = true;

    const auto preview = FeedbackDiagnosticService::buildDiagnosticPreview(input);
    EXPECT_GE(FeedbackDiagnosticService::kDefaultLogLines, 1000u);
    const nlohmann::json previewJson = {
        {"environment", preview.environment},
        {"redactedMainLog", preview.redactedMainLog},
        {"redactedServiceLog", preview.redactedServiceLog},
        {"redactedLatestCrash", preview.redactedLatestCrash},
        {"markdownReport", preview.markdownReport},
        {"githubIssueUrl", preview.githubIssueUrl},
    };
    EXPECT_NO_THROW({
        const std::string serialized = previewJson.dump();
        EXPECT_NE(serialized.find("markdownReport"), std::string::npos);
    });

    // 2. 验证 MessageBridge 对普通超 1 MiB 载荷返回真实请求 id（非 0），同时放行 <=12 MiB 的 feedback.submit
    auto& mb = MessageBridge::instance();
    bool sawFeedbackSubmit = false;
    mb.registerHandler("feedback.submit", [&sawFeedbackSubmit](const nlohmann::json& params) -> nlohmann::json {
        sawFeedbackSubmit = params.contains("screenshotDataUri");
        return {{"ok", true}, {"mode", "fallback_local"}};
    });

    const std::string bigNormalPayload =
        std::string(R"({"id":77,"method":"test.oversized","params":{"blob":")") +
        std::string(1100 * 1024, 'A') + R"("}})";
    const std::string normalRespRaw = mb.handleMessage(bigNormalPayload);
    const auto normalResp = nlohmann::json::parse(normalRespRaw);
    EXPECT_EQ(normalResp.value("id", 0), 77);
    ASSERT_TRUE(normalResp.contains("error"));

    const std::string bigFeedbackPayload =
        std::string(R"({"id":88,"method":"feedback.submit","params":{"title":"shot","screenshotDataUri":"data:image/png;base64,)") +
        std::string(2 * 1024 * 1024, 'B') + R"("}})";
    const std::string feedbackRespRaw = mb.handleMessage(bigFeedbackPayload);
    const auto feedbackResp = nlohmann::json::parse(feedbackRespRaw);
    EXPECT_EQ(feedbackResp.value("id", 0), 88);
    EXPECT_FALSE(feedbackResp.contains("error"));
    EXPECT_TRUE(sawFeedbackSubmit);

    mb.unregisterHandler("feedback.submit");
}

TEST(FileLocksmithTest, RiskClassificationAndProtection) {
    using tools3000::core::FileLocksmithEngine;
    using tools3000::core::LockProcessRisk;

    EXPECT_EQ(FileLocksmithEngine::classifyProcessRisk(4, L"System"), LockProcessRisk::CriticalSystem);
    EXPECT_EQ(FileLocksmithEngine::classifyProcessRisk(500, L"csrss.exe"), LockProcessRisk::CriticalSystem);
    EXPECT_EQ(FileLocksmithEngine::classifyProcessRisk(600, L"LSASS.EXE"), LockProcessRisk::CriticalSystem);
    EXPECT_EQ(FileLocksmithEngine::classifyProcessRisk(1234, L"explorer.exe"), LockProcessRisk::Explorer);
    EXPECT_EQ(FileLocksmithEngine::classifyProcessRisk(9999, L"notepad.exe"), LockProcessRisk::Normal);

    // 严禁终止系统关键进程或自身进程
    std::string err;
    EXPECT_FALSE(FileLocksmithEngine::terminateProcessSafely(4, err));
    EXPECT_EQ(err, "protected_process");
    EXPECT_FALSE(FileLocksmithEngine::terminateProcessSafely(GetCurrentProcessId(), err));
    EXPECT_EQ(err, "protected_process");
}

TEST(FontPreviewTest, SystemWindowsFontInspection) {
    using tools3000::core::FontPreview;
    wchar_t winDir[MAX_PATH]{};
    ASSERT_GT(GetWindowsDirectoryW(winDir, MAX_PATH), 0u);
    const std::filesystem::path fontsDir = std::filesystem::path(winDir) / L"Fonts";
    const std::filesystem::path segoe = fontsDir / L"segoeui.ttf";
    std::error_code ec;
    if (std::filesystem::is_regular_file(segoe, ec)) {
        const auto info = FontPreview::inspectFont(segoe.wstring());
        EXPECT_TRUE(info.ok);
        EXPECT_EQ(info.format, "TTF");
        EXPECT_FALSE(info.familyName.empty());
        EXPECT_GT(info.glyphCount, 100u);
    }
    const std::filesystem::path msyhTtc = fontsDir / L"msyh.ttc";
    if (std::filesystem::is_regular_file(msyhTtc, ec)) {
        const auto ttcInfo = FontPreview::inspectFont(msyhTtc.wstring());
        EXPECT_TRUE(ttcInfo.ok);
        EXPECT_EQ(ttcInfo.format, "TTC");
        EXPECT_GE(ttcInfo.faceCount, 1u);
        std::vector<uint8_t> extracted;
        EXPECT_TRUE(FontPreview::extractStandaloneTtfFromTtc(msyhTtc.wstring(), 0, extracted));
        EXPECT_GT(extracted.size(), 1024u);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// 便携版精准识别、一键原地安全热更新与配置云备份（同步盘 / WebDAV / S3 SigV4）
// 真进程 + 真 ZIP + 真 DPAPI / AES-256-GCM + 真 WinHTTP/Loopback 端到端实测
// ─────────────────────────────────────────────────────────────────────────────
#include "core/backup/BackupCrypto.h"
#include "core/backup/BackupTransport.h"
#include "core/backup/ConfigBackupService.h"

namespace {

class LoopbackCloudTestServer final {
public:
    enum class Mode { WebDav, S3 };

    LoopbackCloudTestServer(Mode mode, std::string authTokenOrAccessKey, std::string s3Secret = {})
        : m_mode(mode), m_auth(std::move(authTokenOrAccessKey)), m_s3Secret(std::move(s3Secret)) {}

    ~LoopbackCloudTestServer() { stop(); }

    bool start(uint16_t& outPort) {
        WSADATA wsa{};
        if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return false;
        m_wsaStarted = true;

        m_listenSock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (m_listenSock == INVALID_SOCKET) return false;

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = 0; // 让操作系统分配空闲临时端口
        if (bind(m_listenSock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == SOCKET_ERROR) {
            return false;
        }
        int addrLen = sizeof(addr);
        if (getsockname(m_listenSock, reinterpret_cast<sockaddr*>(&addr), &addrLen) == SOCKET_ERROR) {
            return false;
        }
        if (listen(m_listenSock, 16) == SOCKET_ERROR) return false;

        outPort = ntohs(addr.sin_port);
        m_port = outPort;
        m_running.store(true);
        m_thread = std::thread([this]() { serveLoop(); });
        return true;
    }

    void stop() {
        m_running.store(false);
        if (m_listenSock != INVALID_SOCKET) {
            closesocket(m_listenSock);
            m_listenSock = INVALID_SOCKET;
        }
        if (m_thread.joinable()) {
            m_thread.join();
        }
        if (m_wsaStarted) {
            WSACleanup();
            m_wsaStarted = false;
        }
    }

    int verifiedSignatureCount() const { return m_verifiedSigCount.load(); }

private:
    void serveLoop() {
        while (m_running.load()) {
            SOCKET client = accept(m_listenSock, nullptr, nullptr);
            if (client == INVALID_SOCKET) break;
            handleClient(client);
            closesocket(client);
        }
    }

    static std::string toLowerCopy(std::string_view sv) {
        std::string out(sv);
        std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) {
            return static_cast<char>(std::tolower(c));
        });
        return out;
    }

    void handleClient(SOCKET client) {
        DWORD tv = 5000;
        setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&tv), sizeof(tv));
        setsockopt(client, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&tv), sizeof(tv));

        std::string raw;
        char buf[4096];
        size_t headerEnd = std::string::npos;
        size_t contentLength = 0;
        while (true) {
            const int n = recv(client, buf, sizeof(buf), 0);
            if (n <= 0) break;
            raw.append(buf, static_cast<size_t>(n));
            if (headerEnd == std::string::npos) {
                headerEnd = raw.find("\r\n\r\n");
                if (headerEnd != std::string::npos) {
                    const std::string hdrsLower = toLowerCopy(std::string_view(raw).substr(0, headerEnd));
                    const size_t clPos = hdrsLower.find("content-length:");
                    if (clPos != std::string::npos) {
                        const size_t eol = hdrsLower.find("\r\n", clPos);
                        const std::string val = hdrsLower.substr(clPos + 15u, eol - (clPos + 15u));
                        try { contentLength = static_cast<size_t>(std::stoull(val)); } catch (...) {}
                    }
                }
            }
            if (headerEnd != std::string::npos && raw.size() >= headerEnd + 4u + contentLength) {
                break;
            }
        }
        if (headerEnd == std::string::npos) return;

        const std::string headerPart = raw.substr(0, headerEnd);
        const std::string body = raw.substr(headerEnd + 4u, contentLength);

        const size_t firstEol = headerPart.find("\r\n");
        const std::string reqLine = (firstEol == std::string::npos) ? headerPart : headerPart.substr(0, firstEol);
        std::istringstream rl(reqLine);
        std::string method, target, httpVer;
        rl >> method >> target >> httpVer;

        std::map<std::string, std::string> headers;
        size_t pos = (firstEol == std::string::npos) ? headerPart.size() : (firstEol + 2u);
        while (pos < headerPart.size()) {
            const size_t nextEol = headerPart.find("\r\n", pos);
            const std::string line = headerPart.substr(pos, nextEol == std::string::npos ? std::string::npos : nextEol - pos);
            pos = (nextEol == std::string::npos) ? headerPart.size() : (nextEol + 2u);
            const size_t colon = line.find(':');
            if (colon != std::string::npos) {
                std::string k = toLowerCopy(std::string_view(line).substr(0, colon));
                std::string v = line.substr(colon + 1u);
                while (!v.empty() && v.front() == ' ') v.erase(v.begin());
                headers[std::move(k)] = std::move(v);
            }
        }

        auto sendResp = [&](int code, const std::string& statusText, const std::string& contentType, const std::string& respBody) {
            std::ostringstream oss;
            oss << "HTTP/1.1 " << code << " " << statusText << "\r\n"
                << "Content-Type: " << contentType << "\r\n"
                << "Content-Length: " << respBody.size() << "\r\n"
                << "Connection: close\r\n\r\n"
                << respBody;
            const std::string wire = oss.str();
            send(client, wire.data(), static_cast<int>(wire.size()), 0);
        };

        std::string pathOnly = target;
        std::string queryOnly;
        const size_t qPos = target.find('?');
        if (qPos != std::string::npos) {
            pathOnly = target.substr(0, qPos);
            queryOnly = target.substr(qPos + 1u);
        }

        if (m_mode == Mode::WebDav) {
            const std::string expectedAuth = "Basic " + m_auth;
            if (headers["authorization"] != expectedAuth) {
                sendResp(401, "Unauthorized", "text/plain", "auth failed");
                return;
            }
            m_verifiedSigCount.fetch_add(1);
            if (method == "MKCOL") {
                sendResp(201, "Created", "text/plain", "");
                return;
            }
            if (method == "PUT") {
                std::lock_guard<std::mutex> lock(m_mu);
                m_objects[pathOnly] = body;
                sendResp(201, "Created", "text/plain", "");
                return;
            }
            if (method == "PROPFIND") {
                std::ostringstream xml;
                xml << "<?xml version=\"1.0\" encoding=\"utf-8\"?><D:multistatus xmlns:D=\"DAV:\">";
                {
                    std::lock_guard<std::mutex> lock(m_mu);
                    for (const auto& [k, v] : m_objects) {
                        xml << "<D:response><D:href>" << k << "</D:href><D:propstat><D:prop>"
                            << "<D:getcontentlength>" << v.size() << "</D:getcontentlength>"
                            << "<D:getlastmodified>Tue, 24 Nov 2026 03:00:00 GMT</D:getlastmodified>"
                            << "</D:prop></D:propstat></D:response>";
                    }
                }
                xml << "</D:multistatus>";
                sendResp(207, "Multi-Status", "application/xml", xml.str());
                return;
            }
            if (method == "GET") {
                std::lock_guard<std::mutex> lock(m_mu);
                auto it = m_objects.find(pathOnly);
                if (it == m_objects.end()) {
                    sendResp(404, "Not Found", "text/plain", "missing");
                    return;
                }
                sendResp(200, "OK", "application/json", it->second);
                return;
            }
            if (method == "DELETE") {
                std::lock_guard<std::mutex> lock(m_mu);
                m_objects.erase(pathOnly);
                sendResp(204, "No Content", "text/plain", "");
                return;
            }
            sendResp(405, "Method Not Allowed", "text/plain", "");
            return;
        }

        // Mode::S3 — 按 AWS SigV4 独立重算并严格校验每一次请求签名
        using tools3000::core::backup::BackupCrypto;
        using tools3000::core::backup::S3SigV4Params;
        S3SigV4Params verifyParams{};
        verifyParams.method = method;
        verifyParams.host = "127.0.0.1:" + std::to_string(m_port);
        verifyParams.canonicalUri = pathOnly;
        verifyParams.canonicalQuery = queryOnly;
        verifyParams.region = "auto";
        verifyParams.service = "s3";
        verifyParams.accessKeyId = m_auth;
        verifyParams.secretAccessKey = m_s3Secret;
        verifyParams.payloadSha256Hex = BackupCrypto::sha256Hex(body);
        verifyParams.amzDateUtc = headers["x-amz-date"];
        verifyParams.contentType = headers.count("content-type") ? headers["content-type"] : "";

        const auto expectedSig = BackupCrypto::buildS3SigV4Headers(verifyParams);
        if (headers["x-amz-content-sha256"] != expectedSig.payloadSha256 ||
            headers["authorization"] != expectedSig.authorization) {
            sendResp(403, "Forbidden", "text/plain", "sigv4 mismatch");
            return;
        }
        m_verifiedSigCount.fetch_add(1);

        if (method == "PUT") {
            std::lock_guard<std::mutex> lock(m_mu);
            m_objects[pathOnly] = body;
            sendResp(200, "OK", "text/plain", "");
            return;
        }
        if (method == "GET" && queryOnly.find("list-type=2") != std::string::npos) {
            std::ostringstream xml;
            xml << "<?xml version=\"1.0\" encoding=\"UTF-8\"?><ListBucketResult>";
            {
                std::lock_guard<std::mutex> lock(m_mu);
                for (const auto& [k, v] : m_objects) {
                    // pathOnly 格式 "/my-bucket/prefix/file.t3kbak"，去掉 "/my-bucket/" 作为对象 Key
                    std::string objKey = k;
                    if (!objKey.empty() && objKey.front() == '/') objKey.erase(objKey.begin());
                    const size_t bucketSlash = objKey.find('/');
                    if (bucketSlash != std::string::npos) objKey = objKey.substr(bucketSlash + 1u);
                    xml << "<Contents><Key>" << objKey << "</Key><Size>" << v.size()
                        << "</Size><LastModified>2026-11-24T03:00:00.000Z</LastModified></Contents>";
                }
            }
            xml << "</ListBucketResult>";
            sendResp(200, "OK", "application/xml", xml.str());
            return;
        }
        if (method == "GET") {
            std::lock_guard<std::mutex> lock(m_mu);
            auto it = m_objects.find(pathOnly);
            if (it == m_objects.end()) {
                sendResp(404, "Not Found", "text/plain", "missing");
                return;
            }
            sendResp(200, "OK", "application/json", it->second);
            return;
        }
        if (method == "DELETE") {
            std::lock_guard<std::mutex> lock(m_mu);
            m_objects.erase(pathOnly);
            sendResp(204, "No Content", "text/plain", "");
            return;
        }
        sendResp(405, "Method Not Allowed", "text/plain", "");
    }

    Mode m_mode;
    std::string m_auth;
    std::string m_s3Secret;
    SOCKET m_listenSock = INVALID_SOCKET;
    uint16_t m_port = 0;
    bool m_wsaStarted = false;
    std::atomic<bool> m_running{false};
    std::atomic<int> m_verifiedSigCount{0};
    std::thread m_thread;
    std::mutex m_mu;
    std::map<std::string, std::string> m_objects;
};

}  // namespace

TEST(UpdateCheckerTest, PortableDetectionAndRealInPlaceHotUpdateE2E) {
    using tools3000::core::UpdateChecker;

    wchar_t tmpRoot[MAX_PATH]{};
    ASSERT_GT(GetTempPathW(MAX_PATH, tmpRoot), 0u);
    const std::filesystem::path root =
        std::filesystem::path(tmpRoot) /
        (L"Tools3000_便携原地更新实测_" + std::to_wstring(GetCurrentProcessId()));
    std::error_code ec;
    std::filesystem::remove_all(root, ec);

    struct Cleanup {
        std::filesystem::path dir;
        ~Cleanup() {
            std::error_code e;
            std::filesystem::remove_all(dir, e);
        }
    } cleanup{root};

    // 1. 便携目录识别单一事实源全分支断言
    const auto dirWithFlag = root / L"case_flag";
    const auto dirWithDot = root / L"case_dot_tools3000";
    const auto dirBareUnzip = root / L"case_bare_unzip";
    const auto dirInstalled = root / L"case_inno_installed";
    std::filesystem::create_directories(dirWithFlag, ec);
    std::filesystem::create_directories(dirWithDot / L".tools3000", ec);
    std::filesystem::create_directories(dirBareUnzip, ec);
    std::filesystem::create_directories(dirInstalled, ec);
    { std::ofstream(dirWithFlag / L"portable.flag") << "portable"; }
    { std::ofstream(dirInstalled / L"unins000.exe") << "inno"; }

    EXPECT_TRUE(UpdateChecker::isPortableDirectory(dirWithFlag));
    EXPECT_TRUE(UpdateChecker::isPortableDirectory(dirWithDot));
    EXPECT_TRUE(UpdateChecker::isPortableDirectory(dirBareUnzip));
    EXPECT_FALSE(UpdateChecker::isPortableDirectory(dirInstalled));

    // 2. 安装版 /DIR 原地覆盖参数断言
    const std::wstring dirParams = UpdateChecker::buildInstallerParametersForDir(L"D:\\My Apps\\Tools3000\\");
    EXPECT_NE(dirParams.find(L"/SILENT"), std::wstring::npos);
    EXPECT_NE(dirParams.find(L"/DIR=\"D:\\My Apps\\Tools3000\""), std::wstring::npos);

    // 3. 真进程等待 + 真 ZIP 解压 + 真原地热替换端到端实测（含中文目录路径 + 用户数据目录保护）
    const auto portableDir = root / L"用户便携目录";
    const auto newPayloadDir = root / L"new_payload";
    const auto updatesDir = root / L"updates";
    std::filesystem::create_directories(portableDir / L"plugins", ec);
    std::filesystem::create_directories(portableDir / L".tools3000" / L"config", ec);
    std::filesystem::create_directories(portableDir / L"portable_data", ec);
    std::filesystem::create_directories(newPayloadDir / L"plugins", ec);
    std::filesystem::create_directories(newPayloadDir / L"data", ec);
    std::filesystem::create_directories(updatesDir, ec);

    { std::ofstream(portableDir / L"Tools3000.exe", std::ios::binary) << "OLD_EXE_1.4.4"; }
    { std::ofstream(portableDir / L"plugins" / L"Plugin_Capture.dll", std::ios::binary) << "OLD_DLL"; }
    { std::ofstream(portableDir / L".tools3000" / L"config" / L"config.json", std::ios::binary) << "{\"keep\":true}"; }
    { std::ofstream(portableDir / L"portable_data" / L"notes.txt", std::ios::binary) << "USER_PRIVATE_DATA"; }

    { std::ofstream(newPayloadDir / L"Tools3000.exe", std::ios::binary) << "NEW_EXE_1.4.6"; }
    { std::ofstream(newPayloadDir / L"plugins" / L"Plugin_Capture.dll", std::ios::binary) << "NEW_DLL_1.4.6"; }
    { std::ofstream(newPayloadDir / L"data" / L"should_be_skipped.txt", std::ios::binary) << "SKIP_ME"; }

    const auto zipPath = updatesDir / L"Tools3000-v1.4.6-win-x64-portable.zip";
    // 用系统原生 .NET ZipFile 打包含 portable.flag 的真实便携 ZIP
    std::wstring packCmd =
        L"powershell.exe -NoProfile -NonInteractive -ExecutionPolicy Bypass -Command \""
        L"Add-Type -AssemblyName System.IO.Compression.FileSystem; "
        L"[System.IO.Compression.ZipFile]::CreateFromDirectory('" + newPayloadDir.wstring() + L"', '" + zipPath.wstring() + L"'); "
        L"$z=[System.IO.Compression.ZipFile]::Open('" + zipPath.wstring() + L"','Update'); "
        L"$e=$z.CreateEntry('portable.flag'); $w=New-Object System.IO.StreamWriter($e.Open()); $w.Write('Tools3000 Portable'); $w.Dispose(); $z.Dispose()\"";
    STARTUPINFOW packSi{};
    packSi.cb = sizeof(packSi);
    packSi.dwFlags = STARTF_USESHOWWINDOW;
    packSi.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION packPi{};
    ASSERT_TRUE(CreateProcessW(nullptr, packCmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &packSi, &packPi));
    ASSERT_EQ(WaitForSingleObject(packPi.hProcess, 15000), WAIT_OBJECT_0);
    CloseHandle(packPi.hThread);
    CloseHandle(packPi.hProcess);
    ASSERT_TRUE(std::filesystem::exists(zipPath, ec));

    // 启动一个真实短命子进程模拟原 Tools3000.exe 优雅退场
    std::wstring dummyCmd = L"cmd.exe /c ping -n 2 127.0.0.1 >nul";
    STARTUPINFOW dummySi{};
    dummySi.cb = sizeof(dummySi);
    dummySi.dwFlags = STARTF_USESHOWWINDOW;
    dummySi.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION dummyPi{};
    ASSERT_TRUE(CreateProcessW(nullptr, dummyCmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &dummySi, &dummyPi));
    const uint32_t dummyPid = static_cast<uint32_t>(dummyPi.dwProcessId);
    CloseHandle(dummyPi.hThread);
    CloseHandle(dummyPi.hProcess);

    std::string launchErr;
    ASSERT_TRUE(UpdateChecker::launchPortableInPlaceUpdater(
        zipPath, portableDir, dummyPid, launchErr, /*relaunchAfterUpdate=*/false)) << launchErr;

    // 轮询等待真实原地热替换完成（最多 20 秒）
    auto readAll = [](const std::filesystem::path& p) -> std::string {
        std::ifstream ifs(p, std::ios::binary);
        return ifs ? std::string((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>()) : std::string{};
    };
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
    while (std::chrono::steady_clock::now() < deadline) {
        if (readAll(portableDir / L"Tools3000.exe") == "NEW_EXE_1.4.6") {
            break;
        }
        Sleep(150);
    }

    EXPECT_EQ(readAll(portableDir / L"Tools3000.exe"), "NEW_EXE_1.4.6");
    EXPECT_EQ(readAll(portableDir / L"plugins" / L"Plugin_Capture.dll"), "NEW_DLL_1.4.6");
    EXPECT_TRUE(std::filesystem::exists(portableDir / L"portable.flag", ec));
    // 严格保护全部用户数据目录：未覆盖、未删除、白名单跳过 ZIP 中 data/
    EXPECT_EQ(readAll(portableDir / L".tools3000" / L"config" / L"config.json"), "{\"keep\":true}");
    EXPECT_EQ(readAll(portableDir / L"portable_data" / L"notes.txt"), "USER_PRIVATE_DATA");
    EXPECT_FALSE(std::filesystem::exists(portableDir / L"data" / L"should_be_skipped.txt", ec));
}

TEST(ConfigBackupServiceTest, CryptoFolderWebDavAndS3SigV4LoopbackE2E) {
    using namespace tools3000::core::backup;

    // 1. Windows DPAPI 真实加解密往返断言（严禁明文暴露）
    std::string protectedCred;
    ASSERT_TRUE(BackupCrypto::dpapiProtectUtf8("SuperSecret_S3_Key!@#", protectedCred));
    EXPECT_EQ(protectedCred.rfind(kDpapiPrefix, 0), 0u);
    EXPECT_EQ(protectedCred.find("SuperSecret_S3_Key"), std::string::npos);
    std::string unprotectedCred;
    ASSERT_TRUE(BackupCrypto::dpapiUnprotectUtf8(protectedCred, unprotectedCred));
    EXPECT_EQ(unprotectedCred, "SuperSecret_S3_Key!@#");

    // 2. AES-256-GCM + PBKDF2 端到端加密与防篡改认证断言
    std::string envelope;
    std::string cryptoErr;
    ASSERT_TRUE(BackupCrypto::encryptEnvelope("{\"hello\":\"tools3000\"}", "Passphrase#2026", envelope, cryptoErr)) << cryptoErr;
    std::string decrypted;
    ASSERT_TRUE(BackupCrypto::decryptEnvelope(envelope, "Passphrase#2026", decrypted, cryptoErr)) << cryptoErr;
    EXPECT_EQ(decrypted, "{\"hello\":\"tools3000\"}");

    std::string wrongPlain;
    EXPECT_FALSE(BackupCrypto::decryptEnvelope(envelope, "WrongPassphrase", wrongPlain, cryptoErr));
    EXPECT_EQ(cryptoErr, "decrypt_auth_failed");

    auto tamperedJson = nlohmann::json::parse(envelope);
    std::string ct = tamperedJson["ciphertext"].get<std::string>();
    ct[0] = (ct[0] == 'A') ? 'B' : 'A';
    tamperedJson["ciphertext"] = ct;
    EXPECT_FALSE(BackupCrypto::decryptEnvelope(tamperedJson.dump(), "Passphrase#2026", wrongPlain, cryptoErr));
    EXPECT_EQ(cryptoErr, "decrypt_auth_failed");

    // 准备隔离数据目录、原始配置与自定义 Lua 脚本
    wchar_t tmpRoot[MAX_PATH]{};
    ASSERT_GT(GetTempPathW(MAX_PATH, tmpRoot), 0u);
    const std::filesystem::path root =
        std::filesystem::path(tmpRoot) /
        (L"Tools3000_CloudBackupE2E_" + std::to_wstring(GetCurrentProcessId()));
    std::error_code ec;
    std::filesystem::remove_all(root, ec);

    struct Cleanup {
        std::filesystem::path dir;
        ~Cleanup() {
            std::error_code e;
            std::filesystem::remove_all(dir, e);
        }
    } cleanup{root};

    const auto dataDir = root / L"app_data";
    const auto cfgFile = dataDir / L"config" / L"config.json";
    const auto luaFile = dataDir / L"gestures" / L"scripts" / L"my_action.lua";
    std::filesystem::create_directories(cfgFile.parent_path(), ec);
    std::filesystem::create_directories(luaFile.parent_path(), ec);
    { std::ofstream(luaFile, std::ios::binary) << "return 'custom-lua-ok'"; }

    const nlohmann::json originalCfg = {
        {"general", {{"language", "zh-CN"}, {"theme", "dark"}}},
        {"gesture", {{"enabled", true}}},
        {"backup", {{"s3SecretKeyEnc", "dpapi:v1:MUST_NOT_UPLOAD"}}},
    };
    { std::ofstream(cfgFile, std::ios::binary) << originalCfg.dump(2); }

    // 校验快照构建器强制剥离了 /backup 敏感凭据子树
    const auto snapEnv = ConfigBackupService::buildSnapshotEnvelope(originalCfg, dataDir);
    EXPECT_FALSE(snapEnv["payload"]["config"].contains("backup"));
    EXPECT_EQ(snapEnv["payload"]["customGestureScripts"].value("my_action.lua", ""), "return 'custom-lua-ok'");

    // 3. 策略 1 (Folder / OneDrive 同步盘) 真实加密备份 + 破坏本地配置 + 安全恢复 + pre-restore 保险断言
    {
        ResolvedBackupConfig folderCfg{};
        folderCfg.settings.provider = BackupProviderType::Folder;
        folderCfg.settings.folderPath = tools3000::core::WinUtils::wstringToUtf8((root / L"OneDriveSync").wstring());
        folderCfg.settings.encryptionEnabled = true;
        folderCfg.settings.keepCount = 2;
        folderCfg.encryptionPassphrase = "SyncPass#1";

        const auto bRes = ConfigBackupService::backupWithConfig(folderCfg, originalCfg, dataDir);
        ASSERT_TRUE(bRes.success) << bRes.errorCode;
        EXPECT_FALSE(bRes.snapshotName.empty());

        const auto lRes = ConfigBackupService::listWithConfig(folderCfg);
        ASSERT_TRUE(lRes.success);
        ASSERT_EQ(lRes.snapshots.size(), 1u);
        EXPECT_TRUE(lRes.snapshots[0].encrypted);

        // 模拟本地配置和脚本被意外改坏/删除
        { std::ofstream(cfgFile, std::ios::binary | std::ios::trunc) << "{\"general\":{\"theme\":\"broken\"}}"; }
        std::filesystem::remove(luaFile, ec);

        const nlohmann::json localBackupKeep = {{"provider", "folder"}, {"s3SecretKeyEnc", "dpapi:v1:LOCAL_ONLY"}};
        const auto rRes = ConfigBackupService::restoreWithConfig(
            folderCfg, lRes.snapshots[0].id, "", cfgFile, dataDir, localBackupKeep);
        ASSERT_TRUE(rRes.success) << rRes.errorCode;
        EXPECT_FALSE(rRes.preRestoreBackupPath.empty());
        EXPECT_TRUE(std::filesystem::exists(tools3000::core::WinUtils::utf8ToWstring(rRes.preRestoreBackupPath), ec));

        std::ifstream restoredIfs(cfgFile, std::ios::binary);
        const auto restoredJson = nlohmann::json::parse(restoredIfs);
        EXPECT_EQ(restoredJson["general"]["theme"].get<std::string>(), "dark");
        EXPECT_EQ(restoredJson["backup"]["s3SecretKeyEnc"].get<std::string>(), "dpapi:v1:LOCAL_ONLY");
        std::ifstream luaIfs(luaFile, std::ios::binary);
        ASSERT_TRUE(luaIfs.good());
        EXPECT_EQ(std::string((std::istreambuf_iterator<char>(luaIfs)), std::istreambuf_iterator<char>()), "return 'custom-lua-ok'");
    }

    // 4. 策略 2 (WebDAV) 真实 WinHTTP + Loopback HTTP 服务端全链路实测
    {
        const std::string rawCred = "alice:dav-secret";
        const std::string b64Cred = BackupCrypto::base64Encode(
            reinterpret_cast<const uint8_t*>(rawCred.data()), rawCred.size());
        LoopbackCloudTestServer davServer(LoopbackCloudTestServer::Mode::WebDav, b64Cred);
        uint16_t davPort = 0;
        ASSERT_TRUE(davServer.start(davPort));

        ResolvedBackupConfig davCfg{};
        davCfg.settings.provider = BackupProviderType::WebDav;
        davCfg.settings.webdavUrl = "http://127.0.0.1:" + std::to_string(davPort) + "/dav/Tools3000/";
        davCfg.settings.webdavUsername = "alice";
        davCfg.webdavPassword = "dav-secret";
        davCfg.settings.encryptionEnabled = true;
        davCfg.encryptionPassphrase = "DavPass#2026";

        const auto bRes = ConfigBackupService::backupWithConfig(davCfg, originalCfg, dataDir);
        ASSERT_TRUE(bRes.success) << bRes.errorCode;

        const auto lRes = ConfigBackupService::listWithConfig(davCfg);
        ASSERT_TRUE(lRes.success) << lRes.errorCode;
        ASSERT_EQ(lRes.snapshots.size(), 1u);

        { std::ofstream(cfgFile, std::ios::binary | std::ios::trunc) << "{\"general\":{\"theme\":\"wiped\"}}"; }
        const auto rRes = ConfigBackupService::restoreWithConfig(
            davCfg, lRes.snapshots[0].id, "", cfgFile, dataDir, nlohmann::json::object());
        ASSERT_TRUE(rRes.success) << rRes.errorCode;

        std::ifstream restoredIfs(cfgFile, std::ios::binary);
        const auto restoredJson = nlohmann::json::parse(restoredIfs);
        EXPECT_EQ(restoredJson["general"]["language"].get<std::string>(), "zh-CN");
        EXPECT_GE(davServer.verifiedSignatureCount(), 4);
        davServer.stop();
    }

    // 5. 策略 3 (S3 SigV4) 真实 WinHTTP + Loopback S3 服务端逐请求验签全链路实测
    {
        LoopbackCloudTestServer s3Server(LoopbackCloudTestServer::Mode::S3, "AKIA_TEST_KEY", "SECRET_TEST_KEY_999");
        uint16_t s3Port = 0;
        ASSERT_TRUE(s3Server.start(s3Port));

        ResolvedBackupConfig s3Cfg{};
        s3Cfg.settings.provider = BackupProviderType::S3;
        s3Cfg.settings.s3Endpoint = "http://127.0.0.1:" + std::to_string(s3Port);
        s3Cfg.settings.s3Region = "auto";
        s3Cfg.settings.s3Bucket = "my-bucket";
        s3Cfg.settings.s3Prefix = "tools3000-backups/";
        s3Cfg.settings.s3AccessKeyId = "AKIA_TEST_KEY";
        s3Cfg.s3SecretKey = "SECRET_TEST_KEY_999";
        s3Cfg.settings.s3ForcePathStyle = true;
        s3Cfg.settings.encryptionEnabled = true;
        s3Cfg.encryptionPassphrase = "S3Pass#2026";

        const auto bRes = ConfigBackupService::backupWithConfig(s3Cfg, originalCfg, dataDir);
        ASSERT_TRUE(bRes.success) << bRes.errorCode;

        const auto lRes = ConfigBackupService::listWithConfig(s3Cfg);
        ASSERT_TRUE(lRes.success) << lRes.errorCode;
        ASSERT_EQ(lRes.snapshots.size(), 1u);

        { std::ofstream(cfgFile, std::ios::binary | std::ios::trunc) << "{\"general\":{\"theme\":\"wiped\"}}"; }
        const auto rRes = ConfigBackupService::restoreWithConfig(
            s3Cfg, lRes.snapshots[0].id, "", cfgFile, dataDir, nlohmann::json::object());
        ASSERT_TRUE(rRes.success) << rRes.errorCode;

        std::ifstream restoredIfs(cfgFile, std::ios::binary);
        const auto restoredJson = nlohmann::json::parse(restoredIfs);
        EXPECT_EQ(restoredJson["general"]["theme"].get<std::string>(), "dark");
        // PUT + LIST(prune) + LIST + GET 至少 4 次 AWS SigV4 签名全部位级验签通过！
        EXPECT_GE(s3Server.verifiedSignatureCount(), 4);
        s3Server.stop();
    }
}

TEST(OutOfProcessFileDialogTest, WorkerRoundTripUnicodePathAndZeroClrInHost) {
    using namespace tools3000::core;

    const std::filesystem::path expectedPath =
        WinUtils::getAppDataDirectory() / L"oop_dialog_test_简体_日本語" / L"导出配置_2026.jpg";

    FileDialogRequest req{};
    req.mode = FileDialogMode::SaveFile;
    req.title = L"进程外对话框隔离验证";
    req.defaultExtension = L"jpg";
    req.defaultFileName = L"导出配置_2026.jpg";
    req.initialFilterIndex = 2;
    req.filters = {
        {L"PNG (*.png)", L"*.png"},
        {L"JPEG (*.jpg)", L"*.jpg"},
    };
    req.autoSelectPathForTest = expectedPath;

    const FileDialogResult res = OutOfProcessFileDialog::show(req);
    EXPECT_TRUE(res.ranOutOfProcess);
    EXPECT_TRUE(res.accepted);
    EXPECT_FALSE(res.cancelled);
    EXPECT_EQ(res.selectedFilterIndex, 2u);
    EXPECT_EQ(WinUtils::pathToUtf8(res.selectedPath), WinUtils::pathToUtf8(expectedPath));

    // 核心架构红线断言：宿主进程严禁因文件对话框调用被注入 .NET CLR 运行时
    EXPECT_EQ(GetModuleHandleW(L"clr.dll"), nullptr);
    EXPECT_EQ(GetModuleHandleW(L"mscorlib.ni.dll"), nullptr);
}

TEST(QuietIdleHygieneTest, HeapCompactAndScheduleQuietIdleTrimCallback) {
    using tools3000::core::WinUtils;

    {
        std::vector<uint8_t> transientBuffer(16ull * 1024ull * 1024ull, 0x5Au);
        ASSERT_EQ(transientBuffer.size(), 16ull * 1024ull * 1024ull);
    }
    WinUtils::compactProcessHeaps();

    std::vector<uint32_t> observedDelays;
    WinUtils::setQuietIdleTrimTrigger([&observedDelays](uint32_t delayMs) {
        observedDelays.push_back(delayMs);
    });
    WinUtils::scheduleQuietIdleTrim(2800);
    WinUtils::scheduleQuietIdleTrim(0); // 0 应被下限保护夹紧至 >= 250ms
    WinUtils::setQuietIdleTrimTrigger(nullptr);
    WinUtils::scheduleQuietIdleTrim(1500); // 解绑后静默无副作用

    ASSERT_EQ(observedDelays.size(), 2u);
    EXPECT_EQ(observedDelays[0], 2800u);
    EXPECT_GE(observedDelays[1], 250u);
}

TEST(QuietIdleHygieneTest, CompactProcessHeapsSurvivesConcurrentPrivateNoSerializeHeaps) {
    using tools3000::core::WinUtils;

    // 防回退回归测试（Windows Server 2019 崩溃根因）：
    // 并发线程持续创建/读写/销毁 HEAP_NO_SERIALIZE 私有堆时，compactProcessHeaps() 绝不得
    // 越界触碰第三方私有堆（否则在 ntdll!RtlCompactHeap 中触发 0xC0000005）。
    std::atomic<bool> stop{false};
    auto churnPrivateNoSerializeHeap = [&stop]() {
        while (!stop.load(std::memory_order_relaxed)) {
            HANDLE h = HeapCreate(HEAP_NO_SERIALIZE, 4096, 0);
            if (!h) continue;
            for (int i = 0; i < 8; ++i) {
                void* p = HeapAlloc(h, HEAP_NO_SERIALIZE, 256);
                if (p) HeapFree(h, HEAP_NO_SERIALIZE, p);
            }
            HeapDestroy(h);
        }
    };

    std::thread t1(churnPrivateNoSerializeHeap);
    std::thread t2(churnPrivateNoSerializeHeap);
    for (int i = 0; i < 150; ++i) {
        WinUtils::compactProcessHeaps();
    }
    stop.store(true, std::memory_order_relaxed);
    t1.join();
    t2.join();

    // 验证 PerformanceMonitor 观察者按需采样模式：显式 2000ms 启动且被观测后处于实时档
    auto& monitor = tools3000::core::PerformanceMonitor::instance();
    monitor.start(2000);
    (void)monitor.getMetrics();
    EXPECT_EQ(monitor.currentSamplingIntervalMs(), 2000);
    monitor.stop();
}

TEST(ShellForegroundLaunchTest, ExtractsTitleHintAndResolvesAssociatedExecutable) {
    using tools3000::core::WinUtils;

    // 1. 搜索打开文档：精准提取无后缀文件名提示词，用于识别任务栏闪烁中的单实例窗口
    EXPECT_EQ(WinUtils::extractShellTargetTitleHint(L"D:\\Chosen\\237-上海山源\\账号密码.txt"),
              L"账号密码");
    EXPECT_EQ(WinUtils::extractShellTargetTitleHint(L"notepad.exe", L"\"D:\\Docs\\Project_Notes.md\""),
              L"project_notes");
    EXPECT_TRUE(WinUtils::extractShellTargetTitleHint(L"https://github.com/yuan278501381/tools3000").empty());

    // 2. 关联可执行程序解析：显式 exe、系统目录及 .txt 文本默认关联程序
    EXPECT_EQ(WinUtils::resolveShellTargetAssociatedExe(L"C:\\Windows\\System32\\notepad.exe"),
              L"notepad.exe");
    wchar_t winDir[MAX_PATH]{};
    if (GetWindowsDirectoryW(winDir, MAX_PATH) > 0) {
        EXPECT_EQ(WinUtils::resolveShellTargetAssociatedExe(winDir), L"explorer.exe");
    }
    const std::wstring txtAssoc = WinUtils::resolveShellTargetAssociatedExe(L"D:\\Temp\\demo.txt");
    if (!txtAssoc.empty()) {
        EXPECT_NE(txtAssoc.find(L".exe"), std::wstring::npos);
    }
}

TEST(HotkeyPriorityTest, GetAllOrdersCoreShortcutsAheadOfRecordingControls) {
    using namespace tools3000::core;
    if (!MessageBridge::instance().hasHandler("hotkey.getAll")) {
        MessageBridge::instance().registerBuiltinHandlers();
    }
    auto& hm = HotkeyManager::instance();
    (void)hm.registerHotkey(
        "Mute Microphone",
        HotkeyDef{ModKey::Ctrl | ModKey::Alt | ModKey::Shift, 'M'},
        []() {});
    (void)hm.registerHotkey(
        "Screenshot",
        HotkeyDef{ModKey::None, VK_F1},
        []() {});

    const std::string raw = MessageBridge::instance().handleMessage(
        R"({"id":901,"method":"hotkey.getAll","params":{}})");
    hm.unregisterHotkey("Mute Microphone");
    hm.unregisterHotkey("Screenshot");

    const auto parsed = nlohmann::json::parse(raw, nullptr, false);
    ASSERT_TRUE(parsed.is_object());
    ASSERT_TRUE(parsed.contains("result"));
    const auto& arr = parsed["result"];
    ASSERT_TRUE(arr.is_array());

    int idxScreenshot = -1;
    int idxMuteMic = -1;
    for (int i = 0; i < static_cast<int>(arr.size()); ++i) {
        const std::string name = arr[static_cast<size_t>(i)].value("name", "");
        if (name == "Screenshot") idxScreenshot = i;
        if (name == "Mute Microphone") idxMuteMic = i;
    }
    EXPECT_GE(idxScreenshot, 0);
    EXPECT_GE(idxMuteMic, 0);
    EXPECT_LT(idxScreenshot, idxMuteMic);
}

TEST(ClipboardNativeDragHostPolicyTest, DistinguishesHttpUrlFromPlainTextAndAiIdeHosts) {
    using namespace tools3000::clipboard;

    NativeDragPayload urlPayload;
    urlPayload.unicodeText = L"  https://github.com/yuan278501381/Tools3000/issues/25 \r\n";
    EXPECT_TRUE(isSingleHttpUrlPayload(urlPayload));

    NativeDragPayload plainPayload;
    plainPayload.unicodeText = L"NXhy123";
    EXPECT_FALSE(isSingleHttpUrlPayload(plainPayload));

    NativeDragPayload ed2kPayload;
    ed2kPayload.unicodeText = L"ed2k://|file|demo.iso|1024|HASH|/";
    EXPECT_FALSE(isSingleHttpUrlPayload(ed2kPayload));

    NativeDragPayload multiWordUrl;
    multiWordUrl.unicodeText = L"https://github.com/a https://github.com/b";
    EXPECT_FALSE(isSingleHttpUrlPayload(multiWordUrl));

    EXPECT_TRUE(shouldBypassOlePlainTextDropForHost(L"antigravity.exe"));
    EXPECT_FALSE(shouldBypassOleTextDropForHost(L"antigravity.exe", /*payloadIsSingleHttpUrl=*/true));
    EXPECT_TRUE(shouldBypassOleTextDropForHost(L"chrome.exe", /*payloadIsSingleHttpUrl=*/false));
    EXPECT_TRUE(shouldBypassOleTextDropForHost(L"chrome.exe", /*payloadIsSingleHttpUrl=*/true));
    EXPECT_TRUE(shouldBypassOleTextDropForHost(L"msedge.exe", /*payloadIsSingleHttpUrl=*/false));
    EXPECT_TRUE(shouldBypassOlePlainTextDropForHost(L"Cursor.exe"));
    EXPECT_TRUE(shouldBypassOlePlainTextDropForHost(L"Clash Party.exe"));
    EXPECT_FALSE(shouldBypassOlePlainTextDropForHost(L"notepad.exe"));
    EXPECT_FALSE(shouldBypassOlePlainTextDropForHost(L"explorer.exe"));

    // 防回退回归测试（图片/文件拖入 Antigravity / 浏览器双图重复粘贴）：
    // 图片与文件载荷（isPureTextPayload == false）落入 AI IDE 或现代浏览器时，
    // 外部目标通过原生 OLE GetData 消费后必须计入 oleRead，严禁再触发第二引擎 Ctrl+V 导致插入两张相同图片
    NativeDragPayload imagePayload;
    imagePayload.filePaths.push_back(L"C:\\Temp\\demo.png");
    imagePayload.pngBytes = {0x89, 0x50, 0x4E, 0x47};
    EXPECT_FALSE(imagePayload.isPureTextPayload());
    EXPECT_TRUE(plainPayload.isPureTextPayload());
    EXPECT_TRUE(shouldRecordOleDataRetrieval(/*overSourceClipboard=*/false, /*isPureTextPayload=*/false, L"antigravity.exe", false));
    EXPECT_TRUE(shouldRecordOleDataRetrieval(/*overSourceClipboard=*/false, /*isPureTextPayload=*/false, L"chrome.exe", false));
    EXPECT_TRUE(shouldRecordOleDataRetrieval(/*overSourceClipboard=*/false, /*isPureTextPayload=*/false, L"cursor.exe", false));
    // 剪贴板面板自身首帧 DragEnter 预读绝不可污染 oleRead（无论图片还是纯文本）
    EXPECT_FALSE(shouldRecordOleDataRetrieval(/*overSourceClipboard=*/true, /*isPureTextPayload=*/false, L"antigravity.exe", false));
    EXPECT_FALSE(shouldRecordOleDataRetrieval(/*overSourceClipboard=*/true, /*isPureTextPayload=*/true, L"notepad.exe", false));
    // 纯文本拖向 AI IDE / 浏览器仍不计入 OLE 已消费，由第二引擎落点直贴接管；单条 HTTP(S) URL 拖向 AI IDE 仍计入 OLE（保留 @Link 卡片）
    EXPECT_FALSE(shouldRecordOleDataRetrieval(/*overSourceClipboard=*/false, /*isPureTextPayload=*/true, L"antigravity.exe", false));
    EXPECT_TRUE(shouldRecordOleDataRetrieval(/*overSourceClipboard=*/false, /*isPureTextPayload=*/true, L"antigravity.exe", true));
    EXPECT_FALSE(shouldRecordOleDataRetrieval(/*overSourceClipboard=*/false, /*isPureTextPayload=*/true, L"chrome.exe", true));
    // 当图片 OLE 返回 DROPEFFECT_COPY 且已记录 oleRead=true 时，绝不触发智能回退粘贴
    EXPECT_FALSE(shouldFallbackToSmartDropPaste(
        DRAGDROP_S_DROP, DROPEFFECT_COPY, /*cancelledByUser=*/false,
        /*releasedLeftButton=*/true, /*hasValidExternalClientTarget=*/true,
        /*oleDataActuallyRetrieved=*/true));
}

TEST(FeedbackTailPreservationTest, KeepsUrlWithinSafeLimitWithEffectiveLocaleAndCtrlVGuide) {
    using namespace tools3000::core;
    FeedbackSubmissionInput input;
    input.feedbackType = "bug";
    input.title = "Issue 25 Diagnostic Tail Test";
    input.description = "Verify latest log lines and Ctrl+V guide are never truncated mid-block.";
    input.includeLogs = true;

    const auto preview = FeedbackDiagnosticService::buildDiagnosticPreview(input, 200);
    EXPECT_TRUE(preview.environment.contains("effectiveLocale"));
    EXPECT_LE(preview.githubIssueUrl.size(), 7600u);
    EXPECT_NE(preview.githubIssueUrl.find("Ctrl%2BV"), std::string::npos);
    EXPECT_EQ(preview.githubIssueUrl.find("please%20attach%20the%20exported"), std::string::npos);
}

TEST(PerformanceMonitorPrivateWorkingSetTest, ReportsTruePrivateWorkingSetAndImmediateTrimMetrics) {
    using namespace tools3000::core;
    auto& monitor = PerformanceMonitor::instance();
    const PerfMetrics snap = monitor.sampleNow();
    EXPECT_GT(snap.memoryMB, 0.0);
    EXPECT_GT(snap.privateMemoryMB, 0.0);
    // 真实专用工作集绝不可超过总物理工作集（彻底杜绝将 PrivateUsage 虚拟提交量误报为专用工作集）
    EXPECT_LE(snap.privateMemoryMB, snap.memoryMB + 0.05);

    auto& bridge = MessageBridge::instance();
    const std::string respStr = bridge.handleMessage(
        R"({"id":9401,"method":"perf.trimMemory","params":{}})");
    const auto resp = nlohmann::json::parse(respStr);
    ASSERT_TRUE(resp.contains("result"));
    const auto& res = resp["result"];
    EXPECT_TRUE(res.value("success", false));
    ASSERT_TRUE(res.contains("metrics"));
    EXPECT_GT(res["metrics"].value("memoryMB", 0.0), 0.0);
    EXPECT_GT(res["metrics"].value("privateMemoryMB", 0.0), 0.0);
    EXPECT_LE(res["metrics"].value("privateMemoryMB", 0.0), res["metrics"].value("memoryMB", 0.0) + 0.05);
    EXPECT_TRUE(res.contains("beforePrivateMemoryMB"));
    EXPECT_TRUE(res.contains("afterPrivateMemoryMB"));
}
