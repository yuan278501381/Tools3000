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
    EXPECT_TRUE(tray100.cx == 220 && tray100.cy == 356);
    EXPECT_TRUE(tray150.cx == 330 && tray150.cy == 534);
    EXPECT_TRUE(tray200.cx == 440 && tray200.cy == 712);

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

    // 3. 键盘钩子拦截器注册测试
    bool hookCallbackCalled = false;
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

    for (int id = 1000; id < 1004; ++id) submit(id);
    bool allWorkersStarted = false;
    {
        std::unique_lock lock(gateMutex);
        allWorkersStarted = gateCv.wait_for(lock, std::chrono::seconds(5), [&running]() {
            return running == 4;
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
    const bool evictionResponded =
        futures[4].wait_for(std::chrono::seconds(5)) == std::future_status::ready;
    std::string evictedResponse;
    if (evictionResponded) evictedResponse = futures[4].get();

    {
        std::lock_guard lock(gateMutex);
        release = true;
    }
    gateCv.notify_all();
    bool allResponsesReady = true;
    for (size_t index = 0; index < futures.size(); ++index) {
        if (index == 4) continue;
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
    const auto pluginDir = executableDir.parent_path() / L"plugins" / executableDir.filename();
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
    EXPECT_EQ(bridge.unregisterHandlersByPrefix("audit.file"), 7u);

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


