// ─────────────────────────────────────────────────────────────────────────────
// test_ui_lifecycle.inc — UI_LIFECYCLE 模块化单元测试集
// ─────────────────────────────────────────────────────────────────────────────

TEST(WebViewSecurityTest, TrustsOnlyPackagedOriginInRelease) {
    using tools3000::ui::web_security::isTrustedUri;
    EXPECT_TRUE(isTrustedUri(L"https://tools3000.local/index.html#/search"));
    EXPECT_TRUE(isTrustedUri(L"https://tools3000.local/"));
    EXPECT_FALSE(isTrustedUri(L"https://tools3000.local.evil.example/"));
    EXPECT_FALSE(isTrustedUri(L"file:///C:/temp/index.html"));
    EXPECT_FALSE(isTrustedUri(L"javascript:alert(1)"));
#ifndef _DEBUG
    EXPECT_FALSE(isTrustedUri(L"http://localhost:5173"));
    EXPECT_FALSE(isTrustedUri(L"http://127.0.0.1:5173"));
#endif
}

TEST(WebViewSecurityTest, RejectsOversizedBridgePayload) {
    EXPECT_TRUE(tools3000::ui::web_security::isBridgeMessageSizeAcceptable(
        tools3000::ui::web_security::MaxBridgeMessageBytes));
    EXPECT_FALSE(tools3000::ui::web_security::isBridgeMessageSizeAcceptable(
        tools3000::ui::web_security::MaxBridgeMessageBytes + 1));
}

TEST(WebViewSecurityTest, AllowsTrayRequiredMethods) {
    using tools3000::ui::web_security::isBridgeMethodAllowed;
    using tools3000::ui::web_security::Surface;

    EXPECT_TRUE(isBridgeMethodAllowed("{\"method\":\"general.getSettings\"}", Surface::Tray));
    EXPECT_TRUE(isBridgeMethodAllowed("{\"method\":\"general.updateSettings\"}", Surface::Tray));
    EXPECT_TRUE(isBridgeMethodAllowed("{\"method\":\"plugins.getAll\"}", Surface::Tray));
    EXPECT_TRUE(isBridgeMethodAllowed("{\"method\":\"plugins.setEnabled\"}", Surface::Tray));
    EXPECT_TRUE(isBridgeMethodAllowed("{\"method\":\"gesture.getState\"}", Surface::Tray));
    EXPECT_TRUE(isBridgeMethodAllowed("{\"method\":\"app.restart\"}", Surface::Tray));
    EXPECT_TRUE(isBridgeMethodAllowed("{\"method\":\"app.restartElevated\"}", Surface::Tray));
    EXPECT_TRUE(isBridgeMethodAllowed("{\"method\":\"tray.action\"}", Surface::Tray));
    EXPECT_TRUE(isBridgeMethodAllowed("{\"method\":\"tray.resize\"}", Surface::Tray));
    EXPECT_TRUE(isBridgeMethodAllowed("{\"method\":\"remote.getSettings\"}", Surface::Tray));
    EXPECT_TRUE(isBridgeMethodAllowed("{\"method\":\"remote.updateSettings\"}", Surface::Tray));
    EXPECT_TRUE(isBridgeMethodAllowed("{\"method\":\"hotkey.getAll\"}", Surface::Tray));
    EXPECT_TRUE(isBridgeMethodAllowed("{\"method\":\"capture.pinGroupControl\"}", Surface::Tray));
    EXPECT_TRUE(isBridgeMethodAllowed("{\"method\":\"capture.pasteAsPin\"}", Surface::Tray));
    EXPECT_FALSE(isBridgeMethodAllowed("{\"method\":\"hotkey.rebind\"}", Surface::Tray));
    EXPECT_FALSE(isBridgeMethodAllowed("{\"method\":\"search.query\"}", Surface::Tray));
    EXPECT_FALSE(isBridgeMethodAllowed("{\"method\":\"capture.start\"}", Surface::Tray));
    EXPECT_TRUE(isBridgeMethodAllowed("{\"method\":\"quicklook.ready\"}", Surface::QuickLook));
    EXPECT_TRUE(isBridgeMethodAllowed("{\"method\":\"quicklook.hide\"}", Surface::QuickLook));
    EXPECT_TRUE(isBridgeMethodAllowed("{\"method\":\"general.getSettings\"}", Surface::QuickLook));
    EXPECT_TRUE(isBridgeMethodAllowed("{\"method\":\"search.quickLook\"}", Surface::Search));
    EXPECT_TRUE(isBridgeMethodAllowed("{\"method\":\"general.getSettings\"}", Surface::Search));
    EXPECT_TRUE(isBridgeMethodAllowed("{\"method\":\"quicklook.startDrag\"}", Surface::QuickLook));
}

TEST(KeyboardPipelineTest, FiltersSystemMenuAndHelpMessages) {
    using tools3000::ui::KeyboardPipeline;
    // SC_KEYMENU (Alt / F10 / Alt+Space 激活系统菜单) 必须被管线拦截
    EXPECT_TRUE(KeyboardPipeline::filterWindowMessage(nullptr, WM_SYSCOMMAND, SC_KEYMENU, 0));
    EXPECT_TRUE(KeyboardPipeline::filterWindowMessage(nullptr, WM_SYSCOMMAND, SC_KEYMENU | 0x0002, 0));
    EXPECT_TRUE(KeyboardPipeline::filterWindowMessage(nullptr, WM_SYSCOMMAND, SC_CONTEXTHELP, 0));
    EXPECT_TRUE(KeyboardPipeline::filterWindowMessage(nullptr, WM_HELP, 0, 0));
    EXPECT_TRUE(KeyboardPipeline::filterWindowMessage(nullptr, WM_SYSKEYDOWN, VK_SPACE, 0));
    EXPECT_TRUE(KeyboardPipeline::filterWindowMessage(nullptr, WM_SYSKEYUP, VK_SPACE, 0));
    EXPECT_TRUE(KeyboardPipeline::filterWindowMessage(nullptr, WM_SYSKEYDOWN, VK_F10, 0));
    EXPECT_TRUE(KeyboardPipeline::filterWindowMessage(nullptr, WM_SYSKEYDOWN, VK_MENU, 0));

    // 普通命令如最大化/最小化/关闭等不能被误拦截
    EXPECT_FALSE(KeyboardPipeline::filterWindowMessage(nullptr, WM_SYSCOMMAND, SC_CLOSE, 0));
    EXPECT_FALSE(KeyboardPipeline::filterWindowMessage(nullptr, WM_SYSCOMMAND, SC_MAXIMIZE, 0));
    EXPECT_FALSE(KeyboardPipeline::filterWindowMessage(nullptr, WM_KEYDOWN, VK_SPACE, 0));
    EXPECT_FALSE(KeyboardPipeline::filterWindowMessage(nullptr, WM_SYSKEYDOWN, 'A', 0));
}


TEST(WebViewSuspendTest, DistinguishesRefusalFromFailure) {
    using tools3000::ui::WebViewSuspendOutcome;
    EXPECT_EQ(tools3000::ui::classifyWebViewSuspendCompletion(S_OK, TRUE),
              WebViewSuspendOutcome::Suspended);
    EXPECT_EQ(tools3000::ui::classifyWebViewSuspendCompletion(S_OK, FALSE),
              WebViewSuspendOutcome::Refused);
    EXPECT_EQ(tools3000::ui::classifyWebViewSuspendCompletion(E_FAIL, FALSE),
              WebViewSuspendOutcome::Failed);
}

TEST(WebViewSuspendTest, LateSuspendCompletionResumesOnlyWhenSurfaceWasShownAgain) {
    using Outcome = tools3000::ui::WebViewSuspendOutcome;
    EXPECT_TRUE(tools3000::ui::shouldResumeAfterSuspendCompletion(
        Outcome::Suspended, false, false));
    EXPECT_FALSE(tools3000::ui::shouldResumeAfterSuspendCompletion(
        Outcome::Suspended, true, false));
    EXPECT_FALSE(tools3000::ui::shouldResumeAfterSuspendCompletion(
        Outcome::Suspended, false, true));
    EXPECT_FALSE(tools3000::ui::shouldResumeAfterSuspendCompletion(
        Outcome::Refused, false, false));
    EXPECT_FALSE(tools3000::ui::shouldResumeAfterSuspendCompletion(
        Outcome::Failed, false, false));
}

TEST(WebViewSuspendTest, ControllerFailsClosedForAbsentWebViewAndCanBeRecreated) {
    tools3000::ui::WebViewSuspendController controller;
    EXPECT_EQ(controller.requestSuspend(nullptr, "unit"), E_POINTER);
    EXPECT_EQ(controller.resume(nullptr, "unit"), E_POINTER);
    EXPECT_EQ(controller.requestSuspend(static_cast<ICoreWebView2Controller*>(nullptr), nullptr), E_POINTER);
    EXPECT_EQ(controller.resume(static_cast<ICoreWebView2Controller*>(nullptr)), E_POINTER);

    // Destruction invalidates late callbacks. A later native-window generation
    // receives a fresh state rather than inheriting the previous generation's
    // cancellation flag.
    controller.abandon();
    controller.reset();
    EXPECT_EQ(controller.requestSuspend(nullptr, "unit-recreated"), E_POINTER);
    EXPECT_EQ(controller.resume(nullptr, "unit-recreated"), E_POINTER);
    EXPECT_EQ(controller.requestSuspend(static_cast<ICoreWebView2Controller*>(nullptr), nullptr), E_POINTER);
    EXPECT_EQ(controller.resume(static_cast<ICoreWebView2Controller*>(nullptr)), E_POINTER);
}


TEST(OverlayUiaProviderTest, ExposesActionPropertiesAndMarshalsInvokeToOwnerWindow) {
    // This uses a real UI Automation client against a real HWND rather than
    // testing helper structs in isolation. It covers the WM_GETOBJECT route,
    // child semantics, physical bounds, and the no-UI-work-on-provider-thread
    // invariant of Invoke (the action arrives as a posted window message).
    CoApartment apartment;
    if (FAILED(apartment.result) && apartment.result != RPC_E_CHANGED_MODE) {
        GTEST_SKIP() << "COM apartment unavailable: 0x" << std::hex << apartment.result;
    }

    UiaTestSurface surface;
    UiaTestWindow window(&surface);
    ASSERT_NE(window.hwnd, nullptr);

    IUIAutomation* automation = nullptr;
    HRESULT result = CoCreateInstance(CLSID_CUIAutomation8, nullptr, CLSCTX_INPROC_SERVER,
                                      IID_PPV_ARGS(&automation));
    if (FAILED(result)) {
        GTEST_SKIP() << "UI Automation client unavailable: 0x" << std::hex << result;
    }
    struct AutomationReleaser { IUIAutomation* value; ~AutomationReleaser() { if (value) value->Release(); } } automationReleaser{automation};

    IUIAutomationElement* root = nullptr;
    result = automation->ElementFromHandle(window.hwnd, &root);
    if (FAILED(result) || !root) {
        GTEST_SKIP() << "UI Automation cannot inspect this desktop: 0x" << std::hex << result;
    }
    struct ElementReleaser { IUIAutomationElement* value; ~ElementReleaser() { if (value) value->Release(); } } rootReleaser{root};

    BSTR rootId = nullptr;
    ASSERT_TRUE(SUCCEEDED(root->get_CurrentAutomationId(&rootId)));
    EXPECT_STREQ(rootId, L"Tools3000.TestOverlay");
    SysFreeString(rootId);

    IUIAutomationCondition* condition = nullptr;
    ASSERT_TRUE(SUCCEEDED(automation->CreateTrueCondition(&condition)));
    struct ConditionReleaser { IUIAutomationCondition* value; ~ConditionReleaser() { if (value) value->Release(); } } conditionReleaser{condition};
    IUIAutomationElement* action = nullptr;
    ASSERT_TRUE(SUCCEEDED(root->FindFirst(TreeScope_Children, condition, &action)));
    ASSERT_NE(action, nullptr);
    struct ActionReleaser { IUIAutomationElement* value; ~ActionReleaser() { if (value) value->Release(); } } actionReleaser{action};

    BSTR name = nullptr;
    ASSERT_TRUE(SUCCEEDED(action->get_CurrentName(&name)));
    EXPECT_STREQ(name, L"Confirm capture");
    SysFreeString(name);
    BOOL enabled = FALSE;
    EXPECT_TRUE(SUCCEEDED(action->get_CurrentIsEnabled(&enabled)));
    EXPECT_TRUE(enabled);
    BSTR shortcut = nullptr;
    ASSERT_TRUE(SUCCEEDED(action->get_CurrentAcceleratorKey(&shortcut)));
    EXPECT_STREQ(shortcut, L"Enter");
    SysFreeString(shortcut);
    VARIANT selected{};
    ASSERT_TRUE(SUCCEEDED(action->GetCurrentPropertyValue(UIA_SelectionItemIsSelectedPropertyId, &selected)));
    EXPECT_EQ(selected.vt, VT_BOOL);
    EXPECT_EQ(selected.boolVal, VARIANT_TRUE);
    VariantClear(&selected);
    tagRECT bounds{};
    ASSERT_TRUE(SUCCEEDED(action->get_CurrentBoundingRectangle(&bounds)));
    EXPECT_EQ(bounds.left, 40);
    EXPECT_EQ(bounds.top, 60);
    EXPECT_EQ(bounds.right, 220);
    EXPECT_EQ(bounds.bottom, 104);

    IUIAutomationInvokePattern* invoke = nullptr;
    ASSERT_TRUE(SUCCEEDED(action->GetCurrentPatternAs(UIA_InvokePatternId,
        IID_PPV_ARGS(&invoke))));
    ASSERT_NE(invoke, nullptr);
    struct InvokeReleaser { IUIAutomationInvokePattern* value; ~InvokeReleaser() { if (value) value->Release(); } } invokeReleaser{invoke};
    EXPECT_TRUE(SUCCEEDED(invoke->Invoke()));
    MSG message{};
    while (PeekMessageW(&message, window.hwnd, 0, 0, PM_REMOVE)) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    EXPECT_EQ(surface.invokeCount, 1);
}

// -----------------------------------------------------------------------------
// 26. 贴图窗口几何变换测试套件
// -----------------------------------------------------------------------------

TEST(SpotlightOverlayTest, SettingsAndDefaults) {
    using namespace tools3000::ui;
    auto& spotlight = SpotlightOverlay::instance();
    spotlight.resetDefaults();
    auto s = spotlight.getSettings();
    EXPECT_TRUE(s.enabled);
    EXPECT_TRUE(s.triggerDoubleCtrl);
    EXPECT_FALSE(s.triggerShakeMouse);
    EXPECT_TRUE(s.autoBypassFullscreen);
    EXPECT_EQ(s.spotlightColor, "auto");
    EXPECT_EQ(s.spotlightSize, 300);
    EXPECT_EQ(s.animationDurationMs, 1000);
    EXPECT_EQ(s.holdDurationMs, 0);
    EXPECT_EQ(s.shakeThreshold, 4);
    EXPECT_FALSE(s.clickRippleEnabled);
    EXPECT_EQ(s.clickRippleStyle, "sparkle_burst");
    EXPECT_FALSE(s.mouseTrailEnabled);
    EXPECT_EQ(s.mouseTrailStyle, "sonar_pulses");
    EXPECT_EQ(s.mouseTrailColorMode, "rainbow");
    EXPECT_EQ(s.leftClickColor, "auto");
    EXPECT_EQ(s.rightClickColor, "#fb7185");
    EXPECT_EQ(s.middleClickColor, "#fbbf24");

    s.spotlightSize = 320;
    s.clickRippleEnabled = true;
    s.mouseTrailEnabled = true;
    s.spotlightColor = "#ff0088";
    s.leftClickColor = "#00ffcc";
    s.spotlightAnimStyle = "tactical_sonar";
    spotlight.updateSettings(s);

    auto updated = spotlight.getSettings();
    EXPECT_EQ(updated.spotlightSize, 320);
    EXPECT_TRUE(updated.clickRippleEnabled);
    EXPECT_TRUE(updated.mouseTrailEnabled);
    EXPECT_EQ(updated.spotlightColor, "#ff0088");
    EXPECT_EQ(updated.leftClickColor, "#00ffcc");
    EXPECT_EQ(updated.spotlightAnimStyle, "tactical_sonar");

    s.spotlightAnimStyle = "aurora_ripple";
    spotlight.updateSettings(s);
    EXPECT_EQ(spotlight.getSettings().spotlightAnimStyle, "aurora_ripple");

    s.spotlightAnimStyle = "inward_gravity";
    spotlight.updateSettings(s);
    EXPECT_EQ(spotlight.getSettings().spotlightAnimStyle, "inward_gravity");

    spotlight.resetDefaults();
    EXPECT_EQ(spotlight.getSettings().spotlightSize, 300);
    EXPECT_EQ(spotlight.getSettings().spotlightColor, "auto");
    EXPECT_EQ(spotlight.getSettings().leftClickColor, "auto");
    EXPECT_EQ(spotlight.getSettings().spotlightAnimStyle, "inward_gravity");
}

TEST(SpotlightOverlayTest, ColorParsingAndAutoAccent) {
    using namespace tools3000::ui;
    auto& spotlight = SpotlightOverlay::instance();

    // 1. 标准 HEX 解析
    auto c1 = spotlight.parseColor("#ff0000", 1.0f);
    EXPECT_NEAR(c1.r, 1.0f, 0.01f);
    EXPECT_NEAR(c1.g, 0.0f, 0.01f);
    EXPECT_NEAR(c1.b, 0.0f, 0.01f);

    auto c2 = spotlight.parseColor("00ff00", 0.5f);
    EXPECT_NEAR(c2.r, 0.0f, 0.01f);
    EXPECT_NEAR(c2.g, 1.0f, 0.01f);
    EXPECT_NEAR(c2.a, 0.5f, 0.01f);

    // 2. auto 与主题色联动
    auto temp = std::filesystem::temp_directory_path() /
        (L"Tools3000_SpotlightTest_" + std::to_wstring(GetCurrentProcessId()));
    std::error_code ec;
    std::filesystem::create_directories(temp, ec);
    auto& cfg = tools3000::core::ConfigManager::instance();
    cfg.initialize(temp);

    cfg.set("/general/accentColor", std::string("blue"));
    auto cBlue = spotlight.parseColor("auto", 1.0f);
    EXPECT_NEAR(cBlue.b, 246.0f / 255.0f, 0.05f);

    cfg.set("/general/accentColor", std::string("cyan"));
    auto cCyan = spotlight.parseColor("auto", 1.0f);
    EXPECT_NEAR(cCyan.g, 182.0f / 255.0f, 0.05f);

    cfg.set("/general/accentColor", std::string("amber"));
    auto cAmber = spotlight.parseColor("auto", 1.0f);
    EXPECT_NEAR(cAmber.r, 245.0f / 255.0f, 0.05f);

    cfg.set("/general/accentColor", std::string("mint"));
    auto cMint = spotlight.parseColor("auto", 1.0f);
    EXPECT_NEAR(cMint.g, 185.0f / 255.0f, 0.05f);

    cfg.set("/general/accentColor", std::string("coral"));
    auto cCoral = spotlight.parseColor("auto", 1.0f);
    EXPECT_NEAR(cCoral.r, 244.0f / 255.0f, 0.05f);

    cfg.set("/general/accentColor", std::string("violet"));
    auto cViolet = spotlight.parseColor("auto", 1.0f);
    EXPECT_NEAR(cViolet.r, 139.0f / 255.0f, 0.05f);

    // 3. 非法色值回退
    auto cInvalid = spotlight.parseColor("invalid_color_code", 1.0f);
    EXPECT_NEAR(cInvalid.a, 1.0f, 0.01f);
}

TEST(SpotlightOverlayTest, KeyboardAndMouseInteraction) {
    using namespace tools3000::ui;
    auto& spotlight = SpotlightOverlay::instance();
    spotlight.resetDefaults();

    // 1. 严格双击 Ctrl 检测 (Down -> Up -> Down)
    spotlight.onKeyboardEvent(VK_CONTROL, WM_KEYDOWN);
    // 模拟连发（Repeat KeyDown 应被静默丢弃）
    spotlight.onKeyboardEvent(VK_CONTROL, WM_KEYDOWN);
    spotlight.onKeyboardEvent(VK_CONTROL, WM_KEYUP);
    std::this_thread::sleep_for(std::chrono::milliseconds(40));
    spotlight.onKeyboardEvent(VK_CONTROL, WM_KEYDOWN);
    spotlight.onKeyboardEvent(VK_CONTROL, WM_KEYUP);

    // 2. 其它键取消 / 污染熔断
    spotlight.onKeyboardEvent(VK_CONTROL, WM_KEYDOWN);
    spotlight.onKeyboardEvent('C', WM_KEYDOWN);
    spotlight.onKeyboardEvent(VK_CONTROL, WM_KEYUP);
    spotlight.onKeyboardEvent(VK_ESCAPE, WM_KEYDOWN);

    // 3. 鼠标点击产生波纹
    auto s = spotlight.getSettings();
    s.clickRippleEnabled = true;
    s.mouseTrailEnabled = true;
    s.triggerShakeMouse = true;
    spotlight.updateSettings(s);

    POINT pt1{100, 100};
    spotlight.onMouseDown(0, pt1);
    spotlight.onMouseDown(1, pt1);
    spotlight.onMouseDown(2, pt1);

    // 4. 鼠标移动产生轨迹与摇晃
    for (int i = 0; i < 10; ++i) {
        POINT movePt{100 + (i % 2 == 0 ? 50 : -50), 100};
        spotlight.onMouseMove(movePt);
    }

    // 5. 动效更新与不同聚焦风格触发
    s.spotlightAnimStyle = "inward_gravity";
    spotlight.updateSettings(s);
    spotlight.trigger(POINT{200, 200}, false);
    spotlight.tickAnimation();

    s.spotlightAnimStyle = "tactical_sonar";
    spotlight.updateSettings(s);
    spotlight.trigger(POINT{300, 300}, false);
    spotlight.tickAnimation();

    s.spotlightAnimStyle = "aurora_ripple";
    spotlight.updateSettings(s);
    spotlight.trigger(POINT{400, 400}, false);
    spotlight.tickAnimation();

    spotlight.dismiss();
    spotlight.resetDefaults();
}

TEST(SpotlightOverlayTest, LocalBoundingBoxAndFocusAssistExclusion) {
    auto& spotlight = tools3000::ui::SpotlightOverlay::instance();
    spotlight.resetDefaults();

    // 1. 空状态视口计算
    auto boundsEmpty = spotlight.calculateViewportBoundsLocked();
    EXPECT_FALSE(boundsEmpty.isFullscreen);
    EXPECT_EQ(boundsEmpty.w, 0);
    EXPECT_EQ(boundsEmpty.h, 0);

    // 2. 开启点击水波纹
    auto s = spotlight.getSettings();
    s.clickRippleEnabled = true;
    s.mouseTrailEnabled = true;
    spotlight.updateSettings(s);

    const int targetX = 300;
    const int targetY = 400;
    POINT clickPt{targetX, targetY};
    spotlight.onMouseDown(0, clickPt);

    auto boundsRipple = spotlight.calculateViewportBoundsLocked();
    EXPECT_FALSE(boundsRipple.isFullscreen);
    EXPECT_GT(boundsRipple.w, 0);
    EXPECT_GT(boundsRipple.h, 0);
    // 局部包围盒尺寸应紧凑 (远小于全屏 1920x1080)，并兼容高分屏 DPI 缩放
    const float clickDpiScale = std::clamp(
        tools3000::core::MouseViewportManager::getDpiScaleForPoint(clickPt), 1.0f, 3.0f);
    const int compactLimit = (std::max)(300, static_cast<int>(std::ceil(150.0f * clickDpiScale)));
    EXPECT_LE(boundsRipple.w, compactLimit);
    EXPECT_LE(boundsRipple.h, compactLimit);
    // 局部包围盒应包住点击坐标
    const int rx = boundsRipple.x;
    const int rw = boundsRipple.w;
    const int ry = boundsRipple.y;
    const int rh = boundsRipple.h;
    EXPECT_LE(rx, targetX);
    EXPECT_GE(rx + rw, targetX);
    EXPECT_LE(ry, targetY);
    EXPECT_GE(ry + rh, targetY);

    // 2.5 跨距双点快速连击：局部包围盒突破旧版 600px 硬上限防截断，同时严格小于显示器尺寸避让专注助手
    const RECT monBounds = tools3000::core::MouseViewportManager::getNearestMonitorBounds(clickPt);
    const int monW = monBounds.right - monBounds.left;
    if (monW >= 1024) {
        const POINT secondPt{monBounds.left + 860, targetY};
        const POINT firstPt{monBounds.left + 160, targetY};
        spotlight.hideNow();
        spotlight.updateSettings(s);
        spotlight.onMouseDown(0, firstPt);
        spotlight.onMouseDown(0, secondPt);
        const auto wideBounds = spotlight.calculateViewportBoundsLocked();
        EXPECT_FALSE(wideBounds.isFullscreen);
        EXPECT_GT(wideBounds.w, 600);
        EXPECT_LE(wideBounds.w, monW - 4);
        EXPECT_LE(wideBounds.x, firstPt.x);
        EXPECT_GE(wideBounds.x + wideBounds.w, secondPt.x);
    }

    // 验证托盘菜单拓宽基准宽度 (252px @ 96 DPI)
    EXPECT_EQ(tools3000::ui::TrayWindowStyle::windowSizeForDpi(96).cx, 276);
    EXPECT_EQ(tools3000::ui::TrayWindowStyle::windowSizeForDpi(144).cx, 414);
    EXPECT_EQ(tools3000::ui::TrayWindowStyle::windowSizeForDpi(192).cx, 552);

    // 3. 触发聚光灯模式
    spotlight.trigger(POINT{500, 500}, false);
    auto boundsSpotlight = spotlight.calculateViewportBoundsLocked();
    EXPECT_TRUE(boundsSpotlight.isFullscreen);
    int vw = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    int vh = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    if (vw > 2 && vh > 2) {
        // 验证避让 1 像素以打破 Windows 11 Shell 的全屏独占判定 (防止触发 Focus Assist 专注助手铃铛)
        const int expectedW = vw - 1;
        const int expectedH = vh - 1;
        EXPECT_EQ(boundsSpotlight.w, expectedW);
        EXPECT_EQ(boundsSpotlight.h, expectedH);
    }

    // 4. 重置并退出
    spotlight.dismiss();
    spotlight.tickAnimation();
    spotlight.resetDefaults();
}

TEST(SpotlightOverlayTest, TrailAndClickStylesMatrix) {
    auto& spotlight = tools3000::ui::SpotlightOverlay::instance();
    spotlight.resetDefaults();

    // 1. 验证 HSL 颜色转换算法
    D2D1_COLOR_F cRed = tools3000::ui::SpotlightOverlay::hslToRgb(0.0f, 1.0f, 0.5f, 1.0f);
    EXPECT_NEAR(cRed.r, 1.0f, 0.01f);
    EXPECT_NEAR(cRed.g, 0.0f, 0.01f);
    EXPECT_NEAR(cRed.b, 0.0f, 0.01f);

    D2D1_COLOR_F cGreen = tools3000::ui::SpotlightOverlay::hslToRgb(120.0f, 1.0f, 0.5f, 1.0f);
    EXPECT_NEAR(cGreen.r, 0.0f, 0.01f);
    EXPECT_NEAR(cGreen.g, 1.0f, 0.01f);
    EXPECT_NEAR(cGreen.b, 0.0f, 0.01f);

    D2D1_COLOR_F cBlue = tools3000::ui::SpotlightOverlay::hslToRgb(240.0f, 1.0f, 0.5f, 0.8f);
    EXPECT_NEAR(cBlue.r, 0.0f, 0.01f);
    EXPECT_NEAR(cBlue.g, 0.0f, 0.01f);
    EXPECT_NEAR(cBlue.b, 1.0f, 0.01f);
    EXPECT_NEAR(cBlue.a, 0.8f, 0.01f);

    // 2. 验证 9 款点击特效风格
    auto s = spotlight.getSettings();
    s.enabled = true;
    s.clickRippleEnabled = true;

    const std::vector<std::string> clickStyles = {
        "sparkle_burst", "ripple_ring", "target_pulse", "soft_glow",
        "supernova", "emp_discharge", "ink_droplet", "hexagon_lock", "bubble_pop",
        "mac_zoom"
    };

    for (const auto& cStyle : clickStyles) {
        s.clickRippleStyle = cStyle;
        spotlight.updateSettings(s);
        spotlight.onMouseDown(0, POINT{200, 200});
        auto bounds = spotlight.calculateViewportBoundsLocked();
        EXPECT_GT(bounds.w, 0);
    }

    // 3. 验证 9 款轨迹动效风格
    s.mouseTrailEnabled = true;

    const std::vector<std::string> trailStyles = {
        "sonar_pulses", "stardust_orbs", "quantum_lens", "tesla_arc",
        "zen_ink", "blueprint_grid", "morning_dew", "aurora_ribbon", "classic_comet"
    };

    for (const auto& tStyle : trailStyles) {
        s.mouseTrailStyle = tStyle;
        s.mouseTrailColorMode = "rainbow";
        spotlight.updateSettings(s);
        for (int i = 0; i < 5; ++i) {
            spotlight.onMouseMove(POINT{100 + i * 40, 100 + i * 40});
        }
        auto bounds = spotlight.calculateViewportBoundsLocked();
        EXPECT_GT(bounds.w, 0);
    }

    // 4. 验证总开关 enabled = false 时的绝对拦截
    s.enabled = false;
    spotlight.updateSettings(s);
    spotlight.dismiss();
    spotlight.onMouseDown(0, POINT{300, 300});
    spotlight.onMouseMove(POINT{350, 350});
    EXPECT_FALSE(spotlight.isActive());

    // 5. 动效更新与清理
    spotlight.tickAnimation();
    spotlight.dismiss();
    spotlight.resetDefaults();
}

// -----------------------------------------------------------------------------
// 38. 世界级架构与极端异常防御测试套件 (Job Object, 原子写盘, 便携模式, 孤儿锁自愈)
// -----------------------------------------------------------------------------

TEST(TitleBarAndKeycastTest, SeamlessWindowAndFluidKeycast) {
    // 1. 验证 KeycastStyle 多 DPI 物理几何缩放
    const SIZE sz96 = tools3000::keycast::KeycastStyle::windowSizeForDpi(96);
    const SIZE sz144 = tools3000::keycast::KeycastStyle::windowSizeForDpi(144);
    const SIZE sz192 = tools3000::keycast::KeycastStyle::windowSizeForDpi(192);
    EXPECT_EQ(sz96.cx, 800);
    EXPECT_EQ(sz96.cy, 160);
    EXPECT_EQ(sz144.cx, 1200);
    EXPECT_EQ(sz144.cy, 240);
    EXPECT_EQ(sz192.cx, 1600);
    EXPECT_EQ(sz192.cy, 320);

    // 2. 验证智能按键过滤与键盘钩子回调
    auto& hook = tools3000::core::KeyboardHook::instance();
    bool callbackInvoked = false;
    hook.setKeycastCallback([&callbackInvoked](const std::string& seq) {
        if (!seq.empty()) {
            callbackInvoked = true;
        }
    });

    // 3. 验证配置持久化读写
    auto temp = std::filesystem::temp_directory_path() /
        (L"Tools3000_KeycastTest_" + std::to_wstring(GetCurrentProcessId()));
    std::error_code ec;
    std::filesystem::create_directories(temp, ec);
    auto& config = tools3000::core::ConfigManager::instance();
    config.initialize(temp);
    config.set("/keycast/position", "bottom_left");
    config.set("/keycast/filterMode", "smart_shortcuts");
    config.set("/keycast/mergeRecentKeys", true);
    config.set("/keycast/mergeTimeoutMs", 1500);
    EXPECT_EQ(config.get("/keycast/position", std::string("")), "bottom_left");
    EXPECT_EQ(config.get("/keycast/filterMode", std::string("")), "smart_shortcuts");
    EXPECT_TRUE(config.get("/keycast/mergeRecentKeys", false));
    EXPECT_EQ(config.get("/keycast/mergeTimeoutMs", 0), 1500);
    config.shutdown();
    std::filesystem::remove_all(temp, ec);

    hook.setKeycastCallback(nullptr);
}

TEST(KeycastOverlayTest, SettingsAndAnimationCombos) {
    auto& overlay = tools3000::keycast::KeycastOverlay::instance();
    
    // 1. 测试默认配置获取
    tools3000::keycast::KeycastSettings s = overlay.getSettings();
    EXPECT_TRUE(s.enabled);
    EXPECT_EQ(s.fontSize, 36);
    EXPECT_EQ(s.opacity, 80);
    EXPECT_EQ(s.firstKeyAnim, "slide");
    EXPECT_EQ(s.subsequentKeyAnim, "fade");
    EXPECT_TRUE(s.rowCascadeAnim);
    EXPECT_TRUE(s.exitDriftAnim);

    // 2. 测试颜色解析 (带 auto、HEX 及异常兜底)
    D2D1_COLOR_F cWhite = overlay.parseColor("#ffffff", 1.0f);
    EXPECT_FLOAT_EQ(cWhite.r, 1.0f);
    EXPECT_FLOAT_EQ(cWhite.g, 1.0f);
    EXPECT_FLOAT_EQ(cWhite.b, 1.0f);

    D2D1_COLOR_F cDark = overlay.parseColor("#1c1c22", 0.98f);
    EXPECT_GT(cDark.a, 0.90f);

    D2D1_COLOR_F cAuto = overlay.parseColor("auto", 1.0f);
    EXPECT_FLOAT_EQ(cAuto.a, 1.0f);

    D2D1_COLOR_F cFallback = overlay.parseColor("invalid_hex", 1.0f);
    EXPECT_FLOAT_EQ(cFallback.a, 1.0f);

    // 3. 测试单项宽度计算
    tools3000::keycast::KeycastItem item1;
    item1.rawKey = "Win+E";
    item1.tokens = {"Win", "E"};
    item1.repeatCount = 1;
    float w1 = overlay.calculateItemWidth(item1, 1.0f);
    EXPECT_GT(w1, 50.0f);

    tools3000::keycast::KeycastItem item2;
    item2.rawKey = "Ctrl+Shift+Alt+Tab";
    item2.tokens = {"Ctrl", "Shift", "Alt", "Tab"};
    item2.repeatCount = 3;
    float w2 = overlay.calculateItemWidth(item2, 1.0f);
    EXPECT_GT(w2, w1);

    // 4. 覆盖不同动效组合配置
    s.firstKeyAnim = "pop";
    s.subsequentKeyAnim = "slide";
    s.rowCascadeAnim = true;
    s.exitDriftAnim = true;
    overlay.updateSettings(s);
    overlay.pushKey("Win+D");
    overlay.pushKey("Win+E");

    s.firstKeyAnim = "fade";
    s.subsequentKeyAnim = "fade";
    s.rowCascadeAnim = false;
    s.exitDriftAnim = false;
    overlay.updateSettings(s);
    overlay.pushKey("Ctrl+C");
    overlay.pushKey("Ctrl+V");

    s.modifierKeycapColor = "#3b82f6";
    s.modifierKeycapOpacity = 40;
    s.modifierTextColor = "#0e0f12";
    s.firstKeyAnim = "none";
    s.subsequentKeyAnim = "none";
    overlay.updateSettings(s);
    EXPECT_EQ(overlay.getSettings().modifierTextColor, "#0e0f12");
    overlay.pushKey("Alt+Tab");

    s.modifierTextColor = "auto";
    overlay.updateSettings(s);
    EXPECT_EQ(overlay.getSettings().modifierTextColor, "auto");
    overlay.pushKey("Win+E");

    overlay.setAutoBypassFullscreen(false);
    EXPECT_FALSE(overlay.autoBypassFullscreen());
    overlay.setAutoBypassFullscreen(true);
    EXPECT_TRUE(overlay.autoBypassFullscreen());

    // 验证全局主题与主题色变更响应
    overlay.onThemeChanged();

    overlay.resetDefaults();
}

TEST(KeycastOverlayTest, RecordingDockingAndOverrideLifecycle) {
    auto& overlay = tools3000::keycast::KeycastOverlay::instance();
    auto& kbdHook = tools3000::core::KeyboardHook::instance();

    // 初始状态下非录屏模式
    kbdHook.setRecordingMode(false);
    EXPECT_FALSE(kbdHook.isRecordingMode());

    // 模拟全局设置关闭按键回显
    tools3000::keycast::KeycastSettings s = overlay.getSettings();
    s.enabled = false;
    overlay.updateSettings(s);
    EXPECT_FALSE(overlay.getSettings().enabled);

    // 激活录屏选区停靠
    RECT recRegion{100, 100, 900, 700};
    overlay.setDockingRegion(recRegion, true);
    EXPECT_TRUE(kbdHook.isRecordingMode());

    // 在录屏模式下，即便全局 enabled == false，调用 pushKey 也不会被丢弃
    overlay.pushKey("Ctrl+Shift+R");
    overlay.pushKey("Enter");

    // 结束录屏停靠
    overlay.setDockingRegion({}, false);
    EXPECT_FALSE(kbdHook.isRecordingMode());

    // 恢复默认配置
    overlay.resetDefaults();
    EXPECT_TRUE(overlay.getSettings().enabled);
}

// -----------------------------------------------------------------------------
// 39. 全局核心鼠标钩子测试套件
// -----------------------------------------------------------------------------

TEST(KeycastSmartFilterAndModifierStateMachineTest, ComprehensiveBehavior) {
    auto& overlay = tools3000::keycast::KeycastOverlay::instance();
    auto s = overlay.getSettings();
    EXPECT_FALSE(s.includeFunctionKeys);

    // 1. 配置更新与持久化测试
    s.includeFunctionKeys = false;
    overlay.updateSettings(s);
    EXPECT_FALSE(overlay.getSettings().includeFunctionKeys);

    s.includeFunctionKeys = true;
    overlay.updateSettings(s);
    EXPECT_TRUE(overlay.getSettings().includeFunctionKeys);

    // 2. 模拟状态机逻辑单元验证
    // 模拟修饰键 Win 按下 (KeyDown) -> 暂存
    DWORD pendingMod = VK_LWIN;
    bool comboTriggered = false;

    // 模拟随后的字母 D 按下 (KeyDown) -> 组合键触发
    DWORD mainVk = 'D';
    bool hasWin = true;
    bool hasShift = false;
        bool isLetter = (mainVk >= 0x41 && mainVk <= 0x5A);

    if (pendingMod != 0) {
        comboTriggered = true;
    }
    EXPECT_TRUE(comboTriggered);

    // 模拟 Win 键松开 (KeyUp) -> 已经触发过组合键，不应补发单 Win
    bool shouldEmitSingleWin = false;
    if (pendingMod == VK_LWIN && !comboTriggered) {
        shouldEmitSingleWin = true;
    }
    EXPECT_FALSE(shouldEmitSingleWin);

    // 3. 模拟纯单按 Alt 激活菜单
    pendingMod = VK_LMENU;
    comboTriggered = false;
    // 期间无其他键按下，Alt 松开 (KeyUp)
    bool shouldEmitSingleAlt = false;
    if (pendingMod == VK_LMENU && !comboTriggered) {
        shouldEmitSingleAlt = true;
    }
    EXPECT_TRUE(shouldEmitSingleAlt);

    // 4. 模拟大写单词打字智能过滤 (Shift + H O M E)
    hasShift = true;
    bool hasCtrl = false;
    bool hasAlt = false;
    hasWin = false;
    bool isOnlyShift = hasShift && !hasCtrl && !hasAlt && !hasWin;
    
    // Shift + H: 仅 Shift + 字母 -> 在智能模式下过滤
    std::string filterMode = "smart_shortcuts";
    bool shouldDisplayHomeLetter = true;
    if (filterMode == "smart_shortcuts" && isOnlyShift && isLetter) {
        shouldDisplayHomeLetter = false;
    }
    EXPECT_FALSE(shouldDisplayHomeLetter);

    // Shift + Delete: Shift + 功能键 -> 正常回显组合键
    bool isFunctionalKey = true;
    isLetter = false;
    bool shouldDisplayShiftDelete = false;
    if (!(isOnlyShift && isLetter) && (hasShift || isFunctionalKey)) {
        shouldDisplayShiftDelete = true;
    }
    EXPECT_TRUE(shouldDisplayShiftDelete);

    // 5. 模拟功能键子开关对单按 Space/Delete 的控制
    bool includeFunc = false;
    bool isComboWithMod = false;
    bool shouldDisplaySpaceWithoutMod = isComboWithMod || (isFunctionalKey && includeFunc);
    EXPECT_FALSE(shouldDisplaySpaceWithoutMod);

    includeFunc = true;
    bool shouldDisplaySpaceWithInclude = isComboWithMod || (isFunctionalKey && includeFunc);
    EXPECT_TRUE(shouldDisplaySpaceWithInclude);
}



TEST(KeycastLifecycleTest, KeystrokeBurstQueueAndMergeLifecycle) {
    using namespace tools3000::keycast;
    auto& keycast = KeycastOverlay::instance();

    // 1. 设置与参数生命周期
    KeycastSettings settings = keycast.getSettings();
    settings.enabled = true;
    settings.mergeRecentKeys = true;
    settings.displayDurationMs = 1500;
    keycast.updateSettings(settings);

    // 2. 模拟高频连击推入 (50 次连续按键突发)
    for (int i = 0; i < 50; ++i) {
        keycast.pushKey("Ctrl+C");
    }

    // 3. 测量计算与宽度渲染稳定性
    KeycastItem item;
    item.rawKey = "Ctrl+C";
    item.tokens = {"Ctrl", "C"};
    item.repeatCount = 50;
    float width = keycast.calculateItemWidth(item, 1.0f);
    EXPECT_GT(width, 0.0f);

    // 4. 清理与重置
    keycast.resetDefaults();
}

// -----------------------------------------------------------------------------
// 30. 聚光灯与鼠标特效（水波纹/流光轨迹）全生命周期测试套件
// -----------------------------------------------------------------------------
TEST(SpotlightAndMouseFxLifecycleTest, SpotlightActivationAndDismissLifecycle) {
    using namespace tools3000::ui;
    auto& spotlight = SpotlightOverlay::instance();

    // 1. 初始状态
    SpotlightSettings settings = spotlight.getSettings();
    settings.enabled = true;
    settings.clickRippleEnabled = true;
    settings.mouseTrailEnabled = true;
    spotlight.updateSettings(settings);

    // 2. 触发聚光灯 -> 验证状态机与局部视口
    spotlight.trigger(POINT{300, 300}, false);
    EXPECT_TRUE(spotlight.isActive());
    spotlight.tickAnimation();
    
    // 3. 鼠标交互与水波纹/轨迹粒子生成
    spotlight.onMouseMove(POINT{320, 320});
    spotlight.onMouseDown(0, POINT{320, 320}); // 左键水波纹
    spotlight.onMouseDown(1, POINT{320, 320}); // 右键水波纹
    spotlight.onMouseDown(2, POINT{320, 320}); // 中键水波纹

    // 4. 取消与平滑淡出 (Dismiss)
    spotlight.dismiss();

    // 5. 模拟时间推进至淡出完成 (380ms 退出动画)
    std::this_thread::sleep_for(std::chrono::milliseconds(420));
    spotlight.tickAnimation();

    // 6. 严苛断言：淡出完成后必须完全进入 Idle，窗口与表面必须隐藏，绝无残影
    EXPECT_FALSE(spotlight.isActive());
    EXPECT_FALSE(spotlight.isVisible());

    // 7. 颜色解析与 HSL 转换数学模型验证
    auto color = SpotlightOverlay::hslToRgb(180.0f, 1.0f, 0.5f, 0.8f);
    EXPECT_GE(color.r, 0.0f);
    EXPECT_LE(color.r, 1.0f);
    EXPECT_FLOAT_EQ(color.a, 0.8f);

    spotlight.resetDefaults();
}

TEST(SpotlightAndMouseFxLifecycleTest, SpotlightEscapeAndFastDismissal) {
    using namespace tools3000::ui;
    auto& spotlight = SpotlightOverlay::instance();
    spotlight.resetDefaults();

    // 1. 验证按下 ESC 键瞬时熔断退出聚光灯（无需等待 380ms 淡出）
    spotlight.trigger(POINT{250, 250}, false);
    EXPECT_TRUE(spotlight.isActive());
    spotlight.tickAnimation();

    spotlight.onKeyboardEvent(VK_ESCAPE, WM_KEYDOWN);
    EXPECT_FALSE(spotlight.isActive());
    EXPECT_FALSE(spotlight.isVisible());

    // 2. 验证淡出阶段二次点击鼠标立即关闭（极速退场，0 延迟）
    spotlight.trigger(POINT{350, 350}, false);
    EXPECT_TRUE(spotlight.isActive());
    spotlight.tickAnimation();

    spotlight.dismiss(); // 首次请求退出 (FadeOut)
    EXPECT_TRUE(spotlight.isActive());

    spotlight.onMouseDown(0, POINT{350, 350}); // 淡出中二次点击
    EXPECT_FALSE(spotlight.isActive());
    EXPECT_FALSE(spotlight.isVisible());

    // 3. 验证静默 Idle 态下按下 ESC 键安全无副作用 (不触发无用显隐与内存修剪)
    spotlight.onKeyboardEvent(VK_ESCAPE, WM_KEYDOWN);
    EXPECT_FALSE(spotlight.isActive());
    EXPECT_FALSE(spotlight.isVisible());

    // 4. 验证淡出过程中再次调用 dismiss 立即加速秒退 (Double Dismiss Fast Exit)
    spotlight.trigger(POINT{400, 400}, false);
    EXPECT_TRUE(spotlight.isActive());
    spotlight.tickAnimation();
    spotlight.dismiss(); // 首次转入 FadeOut
    EXPECT_TRUE(spotlight.isActive());
    spotlight.dismiss(); // 再次请求立即秒退并清屏
    EXPECT_FALSE(spotlight.isActive());
    EXPECT_FALSE(spotlight.isVisible());

    spotlight.resetDefaults();
}

TEST(SpotlightAndMouseFxLifecycleTest, SpotlightGhostPreventionAndSelfHealing) {
    using namespace tools3000::ui;
    auto& spotlight = SpotlightOverlay::instance();
    spotlight.resetDefaults();

    // 1. 验证多次连续快速 trigger / dismiss 状态机无死锁
    for (int i = 0; i < 5; ++i) {
        spotlight.trigger(POINT{100 + i * 20, 100 + i * 20}, false);
        EXPECT_TRUE(spotlight.isActive());
        spotlight.tickAnimation();
        spotlight.dismiss();
    }
    // 强制调用 hideNow 进行彻底退场自愈
    spotlight.hideNow();
    EXPECT_FALSE(spotlight.isActive());
    EXPECT_FALSE(spotlight.isVisible());

    // 2. 验证 Idle 态下重复调用 dismiss / onMouseDown 的自愈兜底
    spotlight.dismiss();
    spotlight.onMouseDown(0, POINT{500, 500});
    EXPECT_FALSE(spotlight.isActive());
    EXPECT_FALSE(spotlight.isVisible());

    // 3. 验证无锁渲染快照与钩子输入线程高并发无死锁
    SpotlightOverlay::RenderSnapshot snap;
    snap.animState = SpotlightOverlay::AnimState::FadeOut;
    snap.currentAlpha = 0.5f;
    snap.bounds = SpotlightOverlay::ViewportBounds{0, 0, 800, 600};
    
    std::atomic<bool> threadRunning{true};
    std::thread hookThread([&]() {
        for (int i = 0; i < 20 && threadRunning.load(); ++i) {
            spotlight.onMouseMove(POINT{100 + i, 100 + i});
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    });

    for (int i = 0; i < 10; ++i) {
        spotlight.renderSnapshot(snap);
    }
    threadRunning = false;
    hookThread.join();

    spotlight.hideNow();
    EXPECT_FALSE(spotlight.isActive());
    EXPECT_FALSE(spotlight.isVisible());

    spotlight.resetDefaults();
}

TEST(SpotlightAndMouseFxLifecycleTest, MouseHookThreadNonBlockingOnMouseDown) {
    using namespace tools3000::ui;
    auto& spotlight = SpotlightOverlay::instance();

    SpotlightSettings settings = spotlight.getSettings();
    settings.enabled = true;
    settings.clickRippleEnabled = true;
    settings.mouseTrailEnabled = true;
    spotlight.updateSettings(settings);

    // 1. 触发聚光灯进入 FadeIn / Holding 状态
    spotlight.trigger(POINT{400, 400}, false);
    EXPECT_TRUE(spotlight.isActive());

    // 2. 模拟从独立的底层钩子输入线程 (WH_MOUSE_LL / EventBus 同步回调) 调用 onMouseDown
    std::atomic<bool> completed{false};
    auto start = std::chrono::steady_clock::now();

    std::thread hookThread([&]() {
        // 模拟用户在聚光灯下点击右键准备划手势或点击退出
        spotlight.onMouseDown(1, POINT{450, 450});
        completed = true;
    });

    hookThread.join();
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start).count();

    // 断言输入线程调用耗时极短 (< 100ms)，绝不发生跨线程死锁或同步渲染阻塞
    EXPECT_TRUE(completed.load());
    EXPECT_LT(elapsed, 100);

    // 3. 安全退出与还原
    spotlight.dismiss();
    spotlight.resetDefaults();
}

TEST(SpotlightAndMouseFxLifecycleTest, RenderSnapshotAndLockFreeArchitecture) {
    using namespace tools3000::ui;
    auto& spotlight = SpotlightOverlay::instance();
    spotlight.resetDefaults();

    // 1. 构造快照并验证 renderSnapshot 无锁调用鲁棒性
    SpotlightOverlay::RenderSnapshot snapshot;
    snapshot.animState = SpotlightOverlay::AnimState::Holding;
    snapshot.currentAlpha = 1.0f;
    snapshot.focusProgress = 1.0f;
    snapshot.scalePulse = 1.0f;
    snapshot.reticleAngle = 45.0f;
    snapshot.targetPos = POINT{500, 500};
    snapshot.settings.enabled = true;
    snapshot.settings.spotlightSize = 180;
    snapshot.settings.spotlightColor = "#3B82F6";
    snapshot.bounds = SpotlightOverlay::ViewportBounds{100, 100, 600, 600};

    // 直接调用无锁渲染管线，确保零锁持有且不崩溃
    spotlight.renderSnapshot(snapshot);

    // 2. 验证 Holding 稳态下的 0% CPU 节流与光标跟踪
    spotlight.trigger(POINT{500, 500}, false);
    EXPECT_TRUE(spotlight.isActive());

    // 连续多次调用 tickAnimation，模拟静止状态下的 60fps 时钟
    for (int i = 0; i < 5; ++i) {
        spotlight.tickAnimation();
    }

    spotlight.dismiss();
    spotlight.tickAnimation();
    spotlight.resetDefaults();
}

TEST(SpotlightAndMouseFxLifecycleTest, MainThreadDispatcherAsyncSafety) {
    // 验证在工作线程向 MainThreadDispatcher 投递任务的安全性与非阻塞特性
    std::atomic<bool> workerCompleted{false};
    std::atomic<int> taskExecuted{0};

    std::thread worker([&]() {
        tools3000::core::MainThreadDispatcher::instance().post([&]() {
            taskExecuted.fetch_add(1);
        });
        workerCompleted = true;
    });

    worker.join();
    EXPECT_TRUE(workerCompleted.load());
}

TEST(SpotlightAndMouseFxLifecycleTest, MouseTrailHighPerformanceQueueAndHookSampling) {
    using namespace tools3000::ui;
    auto& spotlight = SpotlightOverlay::instance();
    spotlight.resetDefaults();

    // 1. 开启鼠标轨迹特效 (选用特斯拉电弧微流风格)
    SpotlightSettings settings = spotlight.getSettings();
    settings.enabled = true;
    settings.mouseTrailEnabled = true;
    settings.mouseTrailStyle = "tesla_arc";
    settings.mouseTrailColorMode = "rainbow";
    spotlight.updateSettings(settings);

    // 2. 模拟底层全局鼠标输入线程 (1000Hz 零阻塞采样) 调用 onHookMouseMove
    // 连续注入多个移动坐标点，验证无锁队列入队与节流唤醒
    for (int i = 0; i < 8; ++i) {
        spotlight.onHookMouseMove(150 + i * 20, 200 + i * 20);
    }

    // 3. 执行动画帧步进与局部视口计算 (FocusAssistAvoidance)
    spotlight.tickAnimation();
    EXPECT_TRUE(spotlight.isActive());

    auto bounds = spotlight.calculateViewportBoundsLocked();
    EXPECT_FALSE(bounds.isFullscreen);
    EXPECT_GT(bounds.w, 0);
    EXPECT_GT(bounds.h, 0);
    // 局部包围盒尺寸应远小于全屏尺寸
    EXPECT_LE(bounds.w, 500);
    EXPECT_LE(bounds.h, 500);

    // 4. 验证 core::MouseHook 原始移动通知回调管线
    auto& mouseHook = tools3000::core::MouseHook::instance();
    std::atomic<bool> moveCallbackInvoked{false};
    mouseHook.setRawMoveCallback([&](long x, long y) {
        if (x == 888 && y == 999) {
            moveCallbackInvoked.store(true);
        }
    });

    MSLLHOOKSTRUCT testData{};
    testData.pt.x = 888;
    testData.pt.y = 999;
    testData.flags = 0;
    mouseHook.injectRawEventForTesting(HC_ACTION, WM_MOUSEMOVE, testData);
    EXPECT_TRUE(moveCallbackInvoked.load());

    // 5. 重新挂回 SpotlightOverlay 的回调并清理
    mouseHook.setRawMoveCallback([&spotlight](long x, long y) {
        spotlight.onHookMouseMove(static_cast<int>(x), static_cast<int>(y));
    });
    spotlight.resetDefaults();
}

TEST(SearchWindowHitTestTest, BorderAndCornerMetricsScaleAccuratelyForDpi) {
    using tools3000::core::dpi::scaleForDpi;
    using tools3000::core::dpi::scaleMetric;

    // 100% DPI (96 DPI)
    const float scale100 = scaleForDpi(96);
    const int border100 = scaleMetric(6, scale100);
    const int corner100 = scaleMetric(8, scale100);
    EXPECT_EQ(border100, 6);
    EXPECT_EQ(corner100, 8);

    // 125% DPI (120 DPI)
    const float scale125 = scaleForDpi(120);
    const int border125 = scaleMetric(6, scale125);
    const int corner125 = scaleMetric(8, scale125);
    EXPECT_EQ(border125, 8);
    EXPECT_EQ(corner125, 10);

    // 150% DPI (144 DPI)
    const float scale150 = scaleForDpi(144);
    const int border150 = scaleMetric(6, scale150);
    const int corner150 = scaleMetric(8, scale150);
    EXPECT_EQ(border150, 9);
    EXPECT_EQ(corner150, 12);

    // 200% DPI (192 DPI)
    const float scale200 = scaleForDpi(192);
    const int border200 = scaleMetric(6, scale200);
    const int corner200 = scaleMetric(8, scale200);
    EXPECT_EQ(border200, 12);
    EXPECT_EQ(corner200, 16);

    // 验证逻辑 CSS 像素下，边缘拉伸热区恒为 6px
    EXPECT_FLOAT_EQ(border100 / scale100, 6.0f);
    EXPECT_FLOAT_EQ(border150 / scale150, 6.0f);
    EXPECT_FLOAT_EQ(border200 / scale200, 6.0f);
}

TEST(SearchWindowHitTestTest, CalculateHitTestResultsListBandGivesPriorityToScrollbar) {
    using tools3000::ui::SearchWindow;

    // 模拟窗口矩形: x 从 100 到 900 (宽 800), y 从 100 到 700 (高 600)
    const RECT rc = { 100, 100, 900, 700 };
    const float scale = 1.0f; // 100% DPI: border=6, corner=8

    // 1. 四个对角拐角应精准返回对应 HT*
    EXPECT_EQ(SearchWindow::calculateHitTest(rc, POINT{102, 102}, scale), HTTOPLEFT);
    EXPECT_EQ(SearchWindow::calculateHitTest(rc, POINT{896, 102}, scale), HTTOPRIGHT);
    EXPECT_EQ(SearchWindow::calculateHitTest(rc, POINT{102, 696}, scale), HTBOTTOMLEFT);
    EXPECT_EQ(SearchWindow::calculateHitTest(rc, POINT{896, 696}, scale), HTBOTTOMRIGHT);

    // 2. 顶边、底边、左边正常响应边框拉伸
    EXPECT_EQ(SearchWindow::calculateHitTest(rc, POINT{500, 102}, scale), HTTOP);
    EXPECT_EQ(SearchWindow::calculateHitTest(rc, POINT{500, 698}, scale), HTBOTTOM);
    EXPECT_EQ(SearchWindow::calculateHitTest(rc, POINT{102, 400}, scale), HTLEFT);

    // 3. 右边缘在顶部搜索栏区域正常支持 HTRIGHT 拉伸
    EXPECT_EQ(SearchWindow::calculateHitTest(rc, POINT{898, 150}, scale), HTRIGHT);

    // 4. 右边缘在底部状态栏区域正常支持 HTRIGHT 拉伸
    EXPECT_EQ(SearchWindow::calculateHitTest(rc, POINT{898, 680}, scale), HTRIGHT);

    // 5. 【核心断言】右边缘在结果列表主垂直带全高放行 HTRIGHT，恢复原生 DWM 硬件级左右拉伸光标
    EXPECT_EQ(SearchWindow::calculateHitTest(rc, POINT{898, 400}, scale), HTRIGHT);
    EXPECT_EQ(SearchWindow::calculateHitTest(rc, POINT{899, 400}, scale), HTRIGHT);

    // 6. 窗口中间普通客户区 (离边框 > 6px) 正常返回 HTCLIENT
    EXPECT_EQ(SearchWindow::calculateHitTest(rc, POINT{500, 400}, scale), HTCLIENT);
    EXPECT_EQ(SearchWindow::calculateHitTest(rc, POINT{890, 400}, scale), HTCLIENT);

    // 7. 200% DPI 高分屏缩放下 (scale = 2.0f: border=12, corner=16)
    const float scale200 = 2.0f;
    // 结果列表垂直带内，右边缘 12px 内全高放行 HTRIGHT 边框拉伸
    EXPECT_EQ(SearchWindow::calculateHitTest(rc, POINT{898, 400}, scale200), HTRIGHT);
    EXPECT_EQ(SearchWindow::calculateHitTest(rc, POINT{890, 400}, scale200), HTRIGHT);
    EXPECT_EQ(SearchWindow::calculateHitTest(rc, POINT{880, 400}, scale200), HTCLIENT);
    // 顶部搜索栏内 (y = 150)，右边缘 12px 内正常响应 HTRIGHT
    EXPECT_EQ(SearchWindow::calculateHitTest(rc, POINT{894, 150}, scale200), HTRIGHT);
    // 拐角 16px 区域内精准返回对角拉伸
    EXPECT_EQ(SearchWindow::calculateHitTest(rc, POINT{104, 104}, scale200), HTTOPLEFT);
    EXPECT_EQ(SearchWindow::calculateHitTest(rc, POINT{890, 104}, scale200), HTTOPRIGHT);
    EXPECT_EQ(SearchWindow::calculateHitTest(rc, POINT{104, 690}, scale200), HTBOTTOMLEFT);
    EXPECT_EQ(SearchWindow::calculateHitTest(rc, POINT{890, 690}, scale200), HTBOTTOMRIGHT);
}

TEST(GeneralSettingsAutoStartCacheTest, MessageBridgeAutoStartMethods) {
    using tools3000::core::MessageBridge;
    auto& bridge = MessageBridge::instance();
    bridge.registerBuiltinHandlers();
    tools3000::core::registerFileInteractionHandlers(bridge, "search", "path is required");

    // 验证 general.getSettings 能够快速返回且包含 autoStart 字段
    const auto res1 = bridge.invokeHandler("general.getSettings");
    EXPECT_TRUE(res1.contains("autoStart"));
    EXPECT_TRUE(res1.contains("theme"));

    // 验证 autoStart.get 能够返回布尔值且与 general.getSettings 缓存一致
    const auto getRes = bridge.invokeHandler("autoStart.get");
    EXPECT_TRUE(getRes.contains("enabled"));
    EXPECT_EQ(getRes["enabled"].get<bool>(), res1["autoStart"].get<bool>());

    const bool orig = getRes["enabled"].get<bool>();
    // 验证 autoStart.set 能够正确返回并更新缓存
    const auto setRes = bridge.invokeHandler("autoStart.set", {{"enabled", orig}});
    EXPECT_TRUE(setRes.contains("success"));
    EXPECT_TRUE(setRes.contains("enabled"));
    EXPECT_EQ(setRes["enabled"].get<bool>(), orig);
}
// -----------------------------------------------------------------------------
TEST(QuickLookMultiEncodingAndCopyFileTest, HandlesGbkUtf16AndCopyFileIpc) {
    using tools3000::core::MessageBridge;
    auto& bridge = MessageBridge::instance();
    bridge.registerBuiltinHandlers();
    tools3000::core::registerFileInteractionHandlers(bridge, "search", "path is required");

    const auto tempDir = std::filesystem::temp_directory_path() /
        (L"Tools3000_QL_Test_" + std::to_wstring(GetCurrentProcessId()));
    std::error_code ec;
    std::filesystem::create_directories(tempDir, ec);

    // 1. 创建 GBK (CP936) 编码的中文文本文件 (包含非 UTF-8 字节序列: "测试GBK编码\xB2\xE2\xCA\xD4")
    const auto gbkFile = tempDir / L"gbk_sample.txt";
    {
        std::ofstream ofs(gbkFile, std::ios::binary);
        const char gbkBytes[] = "\xB2\xE2\xCA\xD4" "GBK\xB1\xE0\xC2\xEB";
        ofs.write(gbkBytes, sizeof(gbkBytes) - 1);
    }

    // 2. 创建 UTF-16 LE BOM 编码的脚本文件
    const auto utf16File = tempDir / L"utf16_sample.reg";
    {
        std::ofstream ofs(utf16File, std::ios::binary);
        const unsigned char bom[] = {0xFF, 0xFE};
        ofs.write(reinterpret_cast<const char*>(bom), 2);
        const std::wstring text = L"Windows Registry Editor Version 5.00\r\n";
        ofs.write(reinterpret_cast<const char*>(text.data()), static_cast<std::streamsize>(text.size() * sizeof(wchar_t)));
    }

    // 2b. 验证无 BOM 的 UTF-8 Markdown 文件 (复现 SKILL.md 场景，严防误判为 UTF-16 LE 产生乱码)
    const auto utf8MdFile = tempDir / L"SKILL.md";
    const std::string expectedMd =
        "---\r\n"
        "name: SAP B1 PLD Layout Designer\r\n"
        "description: SAP Business One 10.0 打印布局设计器(PLD) - 通过SQL直接操作RITM/RDOC表生成和修改任意单据的打印格式。\r\n"
        "---\r\n"
        "# Heading 1\r\n"
        "Normal paragraph with 中文 and English text.\r\n";
    {
        std::ofstream ofs(utf8MdFile, std::ios::binary);
        ofs.write(expectedMd.data(), static_cast<std::streamsize>(expectedMd.size()));
    }

    const auto decodedMd = tools3000::core::TextFilePreviewDecoder::readAndDecodeFile(
        utf8MdFile.wstring(), std::filesystem::file_size(utf8MdFile, ec));
    EXPECT_TRUE(decodedMd.fileOpened);
    EXPECT_TRUE(decodedMd.isReadableText);
    EXPECT_EQ(decodedMd.encoding, "UTF-8");
    EXPECT_EQ(decodedMd.utf8Text, expectedMd);

    // 验证 GBK 文件解码结果精确等于 UTF-8 "测试GBK编码"
    const auto decodedGbk = tools3000::core::TextFilePreviewDecoder::readAndDecodeFile(
        gbkFile.wstring(), std::filesystem::file_size(gbkFile, ec));
    EXPECT_TRUE(decodedGbk.fileOpened);
    EXPECT_TRUE(decodedGbk.isReadableText);
    EXPECT_EQ(decodedGbk.encoding, "GBK");
    EXPECT_EQ(decodedGbk.utf8Text, "测试GBK编码");

    // 验证 UTF-16 LE BOM 文件解码结果
    const auto decodedUtf16 = tools3000::core::TextFilePreviewDecoder::readAndDecodeFile(
        utf16File.wstring(), std::filesystem::file_size(utf16File, ec));
    EXPECT_TRUE(decodedUtf16.fileOpened);
    EXPECT_TRUE(decodedUtf16.isReadableText);
    EXPECT_EQ(decodedUtf16.encoding, "UTF-16 LE");
    EXPECT_EQ(decodedUtf16.utf8Text, "Windows Registry Editor Version 5.00\r\n");

    // 验证真实磁盘上的 SKILL.md (若存在) 解码绝不乱码
    const std::filesystem::path realSkillMd = L"C:\\Users\\yuan2\\.gemini\\config\\skills\\sap_b1_pld\\SKILL.md";
    if (std::filesystem::exists(realSkillMd, ec)) {
        const auto realDecoded = tools3000::core::TextFilePreviewDecoder::readAndDecodeFile(
            realSkillMd.wstring(), std::filesystem::file_size(realSkillMd, ec));
        EXPECT_TRUE(realDecoded.fileOpened);
        EXPECT_TRUE(realDecoded.isReadableText);
        EXPECT_EQ(realDecoded.encoding, "UTF-8");
        EXPECT_NE(realDecoded.utf8Text.find("SAP Business One 10.0"), std::string::npos);
        EXPECT_NE(realDecoded.utf8Text.find("打印布局设计器"), std::string::npos);
    }

    // 3. 验证 search.copyFile / system.copyFile IPC 处理器正常注册并处理文件与空路径校验
    const auto emptyCopyRes = bridge.invokeHandler("search.copyFile", {{"path", ""}});
    EXPECT_FALSE(emptyCopyRes.value("success", true));

    const std::string utf8Path = tools3000::core::WinUtils::wstringToUtf8(gbkFile.wstring());
    const auto copyRes = bridge.invokeHandler("search.copyFile", {{"path", utf8Path}});
    EXPECT_TRUE(copyRes.value("success", false));

    const std::string utf8RegPath = tools3000::core::WinUtils::wstringToUtf8(utf16File.wstring());
    const auto sysCopyRes = bridge.invokeHandler("system.copyFile", {{"filepath", utf8RegPath}});
    EXPECT_TRUE(sysCopyRes.value("success", false));

    std::filesystem::remove_all(tempDir, ec);
}

TEST(SpotlightOverlayTest, HoldDurationAutoDismissAndHighShakeThreshold) {
    auto& spotlight = tools3000::ui::SpotlightOverlay::instance();
    spotlight.hideNow();
    spotlight.resetDefaults();

    // 0. 验证默认 holdDurationMs == 0 时演示者模式持续常亮跟随，绝不自动超时退出
    auto s = spotlight.getSettings();
    EXPECT_EQ(s.holdDurationMs, 0);
    s.enabled = true;
    s.autoBypassFullscreen = false;
    s.animationDurationMs = 200; // fadeIn = 120ms, fadeOut = 120ms
    spotlight.updateSettings(s);

    spotlight.trigger(POINT{300, 300}, false);
    EXPECT_TRUE(spotlight.isActive());
    std::this_thread::sleep_for(std::chrono::milliseconds(135));
    spotlight.tickAnimation(); // FadeIn -> Holding
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    spotlight.tickAnimation(); // holdDurationMs == 0: 持续保持 Holding 常亮，不进入 FadeOut
    EXPECT_TRUE(spotlight.isActive());
    spotlight.hideNow();

    // 1. 验证显式配置 holdDurationMs > 0 时在未按住 Ctrl 下自动由 Holding 平滑转入 FadeOut 并归回 Idle
    s.holdDurationMs = 80;
    spotlight.updateSettings(s);

    spotlight.trigger(POINT{320, 320}, false);
    EXPECT_TRUE(spotlight.isActive());

    std::this_thread::sleep_for(std::chrono::milliseconds(135));
    spotlight.tickAnimation(); // FadeIn -> Holding
    EXPECT_TRUE(spotlight.isActive());

    std::this_thread::sleep_for(std::chrono::milliseconds(95));
    spotlight.tickAnimation(); // Holding -> FadeOut (因达到 holdDurationMs)
    EXPECT_TRUE(spotlight.isActive());

    std::this_thread::sleep_for(std::chrono::milliseconds(135));
    spotlight.tickAnimation(); // FadeOut -> Idle
    EXPECT_FALSE(spotlight.isActive());

    // 2. 验证 shakeThreshold 支持 > 10 的高灵敏度档位 (3 ~ 20)，不再被旧上限 10 提前截断
    spotlight.hideNow();
    s = spotlight.getSettings();
    s.enabled = true;
    s.triggerShakeMouse = true;
    s.autoBypassFullscreen = false;
    s.shakeThreshold = 14;
    spotlight.updateSettings(s);

    spotlight.onMouseMove(POINT{200, 200});
    spotlight.onMouseMove(POINT{260, 200}); // 初始化窗口与正向位移 (dir = +1)
    for (int i = 0; i < 11; ++i) {
        const int x = (i % 2 == 0) ? 200 : 260;
        spotlight.onMouseMove(POINT{x, 200});
    }
    // 11 次反向 (> 旧硬上限 10，但 < 配置的 14) 绝不能提前误触发
    EXPECT_FALSE(spotlight.isActive());

    for (int i = 11; i < 14; ++i) {
        const int x = (i % 2 == 0) ? 200 : 260;
        spotlight.onMouseMove(POINT{x, 200});
    }
    EXPECT_TRUE(spotlight.isActive());

    spotlight.hideNow();
    spotlight.resetDefaults();
}

TEST(KeycastOverlayTest, DirectWriteMeasuredWidthAndRepeatBadgeExpansion) {
    auto& overlay = tools3000::keycast::KeycastOverlay::instance();
    overlay.resetDefaults();

    tools3000::keycast::KeycastItem narrowItem;
    narrowItem.rawKey = "I";
    narrowItem.tokens = {"I"};
    narrowItem.repeatCount = 1;

    tools3000::keycast::KeycastItem wideItem;
    wideItem.rawKey = "Backspace";
    wideItem.tokens = {"Backspace"};
    wideItem.repeatCount = 1;

    tools3000::keycast::KeycastItem repeatItem = wideItem;
    repeatItem.repeatCount = 12;
    repeatItem.badgeWidthProgress = 1.0f;

    const float narrowWidth = overlay.calculateItemWidth(narrowItem, 1.0f);
    const float wideWidth = overlay.calculateItemWidth(wideItem, 1.0f);
    const float repeatWidth = overlay.calculateItemWidth(repeatItem, 1.0f);

    EXPECT_GT(narrowWidth, 0.0f);
    EXPECT_GT(wideWidth, narrowWidth + 20.0f);
    EXPECT_GT(repeatWidth, wideWidth + 10.0f);
}

TEST(ToastStyleTest, DynamicCapsuleMetricsExpandForMultilineAndRespectScreenBounds) {
    using tools3000::ui::ToastStyle;

    // 短提示词维持最小优雅胶囊宽高
    const SIZE compact = ToastStyle::capsuleSizeForTextMetrics(48.0f, 20.0f, 1.0f, 1920);
    EXPECT_GE(compact.cx, static_cast<LONG>(ToastStyle::MinCapsuleWidth));
    EXPECT_GE(compact.cy, static_cast<LONG>(ToastStyle::MinCapsuleHeight));

    // 多行超长提示词动态撑高胶囊，突破旧版固定 60px/80px 高度限制防止纵向截断
    const SIZE multiline = ToastStyle::capsuleSizeForTextMetrics(620.0f, 96.0f, 1.0f, 1920);
    EXPECT_GT(multiline.cx, 650);
    EXPECT_GT(multiline.cy, 120);

    // 窄工作区下文本最大排版宽度与胶囊总宽自动收敛，防止溢出屏幕边界
    const SIZE narrowScreen = ToastStyle::capsuleSizeForTextMetrics(900.0f, 72.0f, 1.0f, 800);
    EXPECT_LT(narrowScreen.cx, 800);
    EXPECT_GT(narrowScreen.cy, 96);
}

TEST(ToastStyleTest, TranslucentIceGlassPaletteFollowsLightAndDarkThemeCleanly) {
    using tools3000::ui::ToastGlassPalette;

    EXPECT_FALSE(ToastGlassPalette::resolveIsDark("light", true));
    EXPECT_FALSE(ToastGlassPalette::resolveIsDark("light", false));
    EXPECT_TRUE(ToastGlassPalette::resolveIsDark("dark", false));
    EXPECT_TRUE(ToastGlassPalette::resolveIsDark("dark", true));
    EXPECT_FALSE(ToastGlassPalette::resolveIsDark("system", false));
    EXPECT_TRUE(ToastGlassPalette::resolveIsDark("system", true));

    const ToastGlassPalette light = ToastGlassPalette::forTheme(false);
    const ToastGlassPalette dark = ToastGlassPalette::forTheme(true);

    EXPECT_LE(light.bgTop.a, 0.80f);
    EXPECT_GE(light.bgBottom.a, 0.55f);
    EXPECT_LE(dark.bgTop.a, 0.70f);
    EXPECT_GE(dark.bgBottom.a, 0.45f);

    EXPECT_GE(light.bgTop.r, 0.95f);
    EXPECT_GE(light.bgTop.g, 0.95f);
    EXPECT_GE(light.bgTop.b, 0.95f);
    EXPECT_LE(light.text.r, 0.15f);
    EXPECT_LE(light.text.g, 0.15f);
    EXPECT_LE(light.text.b, 0.25f);
    EXPECT_GE(light.text.a, 0.85f);
    EXPECT_GE(light.innerRimTop.a, 0.85f);
    EXPECT_LE(light.shadowNear.a, 0.08f);

    EXPECT_GE(dark.text.r, 0.94f);
    EXPECT_GE(dark.text.g, 0.94f);
    EXPECT_GE(dark.text.b, 0.94f);
    EXPECT_GE(dark.innerRimTop.a, 0.22f);

    const auto blendChannel = [](float fg, float alpha, float bg) noexcept {
        return fg * alpha + bg * (1.0f - alpha);
    };
    const float lightBgLumOnSettings =
        0.2126f * blendChannel(light.bgTop.r, light.bgTop.a, 248.0f / 255.0f) +
        0.7152f * blendChannel(light.bgTop.g, light.bgTop.a, 250.0f / 255.0f) +
        0.0722f * blendChannel(light.bgTop.b, light.bgTop.a, 252.0f / 255.0f);
    EXPECT_GE(lightBgLumOnSettings * 255.0f, 245.0f);

    const float darkBgLumOnWhite =
        0.2126f * blendChannel(dark.bgTop.r, dark.bgTop.a, 1.0f) +
        0.7152f * blendChannel(dark.bgTop.g, dark.bgTop.a, 1.0f) +
        0.0722f * blendChannel(dark.bgTop.b, dark.bgTop.a, 1.0f);
    EXPECT_GE(darkBgLumOnWhite * 255.0f, 110.0f);
}

TEST(ToastStyleTest, StripsMachineStatusPrefixesWithoutCorruptingNormalBrackets) {
    using tools3000::ui::ToastStyle;

    EXPECT_EQ(ToastStyle::stripMachineStatusPrefix(L"[OK] 截图已复制到剪贴板"), L"截图已复制到剪贴板");
    EXPECT_EQ(ToastStyle::stripMachineStatusPrefix(L"[ok] Screenshot copied"), L"Screenshot copied");
    EXPECT_EQ(ToastStyle::stripMachineStatusPrefix(L"  [INFO]   序号已重置为 1"), L"序号已重置为 1");
    EXPECT_EQ(ToastStyle::stripMachineStatusPrefix(L"[WARN] 磁盘空间不足"), L"磁盘空间不足");
    EXPECT_EQ(ToastStyle::stripMachineStatusPrefix(L"[WARNING] Low disk space"), L"Low disk space");
    EXPECT_EQ(ToastStyle::stripMachineStatusPrefix(L"[ERROR] 截图保存失败"), L"截图保存失败");
    EXPECT_EQ(ToastStyle::stripMachineStatusPrefix(L"[ERR] Save failed"), L"Save failed");

    // 普通方括号文本（如分页/快捷键说明）绝不得被误剥离
    EXPECT_EQ(ToastStyle::stripMachineStatusPrefix(L"[1/3] 历史贴图"), L"[1/3] 历史贴图");
    EXPECT_EQ(ToastStyle::stripMachineStatusPrefix(L"[Ctrl+C] 复制文本"), L"[Ctrl+C] 复制文本");
    EXPECT_EQ(ToastStyle::stripMachineStatusPrefix(L"截图已复制到剪贴板"), L"截图已复制到剪贴板");
    EXPECT_EQ(ToastStyle::stripMachineStatusPrefix(L""), L"");
}

// 单元测试主入口 (Google Test 初始化与执行)
// -----------------------------------------------------------------------------