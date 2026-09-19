// ─────────────────────────────────────────────────────────────────────────────
// test_dialog.inc — DIALOG 模块化单元测试集
// ─────────────────────────────────────────────────────────────────────────────

TEST(PathMemoryManagerTest, AppMemoryAndHistory) {
    auto& mgr = tools3000::dialog::PathMemoryManager::instance();
    mgr.setEnabled(true);
    mgr.setPerAppMemoryEnabled(true);
    mgr.setQuickSwitchEnabled(true);
    mgr.setRibbonEnabled(true);
    mgr.setRibbonPosition("top-right");

    EXPECT_TRUE(mgr.isEnabled());
    EXPECT_TRUE(mgr.isPerAppMemoryEnabled());
    EXPECT_TRUE(mgr.isQuickSwitchEnabled());
    EXPECT_TRUE(mgr.isRibbonEnabled());
    EXPECT_EQ(mgr.getRibbonPosition(), "top-right");

    // 记录应用专属路径
    mgr.clearAppMemories();
    mgr.recordAppPath("Code.exe", "D:\\repo\\tools3000");
    EXPECT_EQ(mgr.getAppPath("Code.exe"), "D:\\repo\\tools3000");
    EXPECT_EQ(mgr.getAppPath("CODE.EXE"), "D:\\repo\\tools3000"); // 同一 EXE 多实例共享键

    mgr.recordAppPath("chrome.exe", "C:\\Downloads");
    EXPECT_EQ(mgr.getAppPath("chrome.exe"), "C:\\Downloads");

    // 固定母工作区优先级
    mgr.setAppFixedWorkspace("Code.exe", "C:\\repo", true);
    EXPECT_EQ(mgr.getEffectiveAppPath("Code.exe"), "C:\\repo");
    mgr.recordAppPath("Code.exe", "C:\\repo\\OpenERP"); // 动态变更不影响固定母工作区
    EXPECT_EQ(mgr.getEffectiveAppPath("Code.exe"), "C:\\repo");
    EXPECT_EQ(mgr.getAppPath("Code.exe"), "C:\\repo\\OpenERP");

    mgr.setAppFixedWorkspace("Code.exe", "C:\\repo", false);
    // 未固定时必须精确恢复上一次目录，不得擅自提升到收藏的“母工作区”。
    EXPECT_EQ(mgr.getEffectiveAppPath("Code.exe"), "C:\\repo\\OpenERP");
    mgr.recordAppPath("Code.exe", "E:\\StandaloneProject");
    EXPECT_EQ(mgr.getEffectiveAppPath("Code.exe"), "E:\\StandaloneProject");

    mgr.removeAppMemory("new-app.exe");
    EXPECT_TRUE(mgr.getEffectiveAppPath("new-app.exe").empty()); // 首次使用保持系统原生目录

    auto memories = mgr.getAllAppMemories();
    EXPECT_GE(memories.size(), 2u);


    mgr.removeAppMemory("chrome.exe");
    EXPECT_TRUE(mgr.getAppPath("chrome.exe").empty());

    // 最近使用文件夹 (MRU)
    mgr.clearRecentPaths();
    mgr.recordRecentPath("C:\\FolderA");
    mgr.recordRecentPath("C:\\FolderB");
    mgr.recordRecentPath("C:\\FolderA"); // 重复置顶
    auto recent = mgr.getRecentPaths(5);
    EXPECT_EQ(recent.size(), 2u);
    EXPECT_EQ(recent[0], "C:\\FolderA");
    EXPECT_EQ(recent[1], "C:\\FolderB");

    // 收藏夹
    mgr.setFavorites({"C:\\repo", "D:\\Workspace"});
    auto favs = mgr.getFavorites();
    EXPECT_EQ(favs.size(), 2u);
    mgr.addFavorite("E:\\Projects");
    EXPECT_EQ(mgr.getFavorites().size(), 3u);
    mgr.removeFavorite("D:\\Workspace");
    EXPECT_EQ(mgr.getFavorites().size(), 2u);

    // 黑名单
    mgr.setBlacklist({"game.exe", "industrial.exe"});
    EXPECT_TRUE(mgr.isProcessBlacklisted("game.exe"));
    EXPECT_TRUE(mgr.isProcessBlacklisted("GAME.EXE"));
    EXPECT_FALSE(mgr.isProcessBlacklisted("code.exe"));
}


TEST(PathMemoryManagerTest, SelectionPathNormalization) {
    auto base = std::filesystem::temp_directory_path() /
                ("tools3000-dialog-memory-" + std::to_string(GetCurrentProcessId()));
    std::error_code ec;
    std::filesystem::remove_all(base, ec);
    ASSERT_TRUE(std::filesystem::create_directories(base / "ChosenFolder", ec));

    const auto selectedFile = base / "photo.psd";
    {
        std::ofstream file(selectedFile, std::ios::binary | std::ios::trunc);
        ASSERT_TRUE(file.good());
    }

    auto asPath = [](const std::string& utf8) {
        return std::filesystem::path(tools3000::core::WinUtils::utf8ToWstring(utf8)).lexically_normal();
    };
    auto asUtf8 = [](const std::filesystem::path& path) {
        return tools3000::core::WinUtils::wstringToUtf8(path.lexically_normal().native());
    };

    // 没有当前浏览目录上下文时，文件夹本身是唯一可靠结果。
    EXPECT_EQ(asPath(tools3000::dialog::PathMemoryManager::directoryForSelection(
                  asUtf8(base / "ChosenFolder"))),
              (base / "ChosenFolder").lexically_normal());

    // 文件夹选择器中高亮当前目录的子文件夹时，记忆它所在的当前目录，
    // 下次仍停留在同一选择层级，而不是钻入上次选择的子文件夹。
    EXPECT_EQ(asPath(tools3000::dialog::PathMemoryManager::directoryForSelection(
                  asUtf8(base / "ChosenFolder"), asUtf8(base))),
              base.lexically_normal());

    // 选择已有文件、保存尚不存在的文件：均记忆所在目录。
    EXPECT_EQ(asPath(tools3000::dialog::PathMemoryManager::directoryForSelection(
                  asUtf8(selectedFile))), base.lexically_normal());
    EXPECT_EQ(asPath(tools3000::dialog::PathMemoryManager::directoryForSelection(
                  asUtf8(base / "future-save.png"))), base.lexically_normal());
    EXPECT_EQ(asPath(tools3000::dialog::PathMemoryManager::directoryForSelection(
                  "relative-save.txt", asUtf8(base))), base.lexically_normal());

    // Windows 多选文件框会把底部输入显示为多个带引号的文件名。该文本不是
    // 单一路径；确认时应可靠回落到当前浏览目录，不能沿用更旧的 EXE 记忆。
    EXPECT_EQ(asPath(tools3000::dialog::PathMemoryManager::directoryForSelection(
                  "\"first.txt\" \"second.txt\"", asUtf8(base))),
              base.lexically_normal());

    std::filesystem::remove_all(base, ec);
}

TEST(ExplorerTrackerTest, PathNormalization) {
    EXPECT_EQ(tools3000::dialog::ExplorerTracker::normalizeFolderPath(L""), "");
    EXPECT_EQ(tools3000::dialog::ExplorerTracker::normalizeFolderPath(L"C:\\Windows"), "C:\\Windows");
    
    // URL 格式路径还原
    EXPECT_FALSE(tools3000::dialog::ExplorerTracker::normalizeFolderPath(L"file:///C:/Windows").empty());
}

TEST(DialogNavigatorTest, NullSafetyAndValidation) {
    EXPECT_FALSE(tools3000::dialog::DialogNavigator::isFileDialog(nullptr));
    EXPECT_EQ(tools3000::dialog::DialogNavigator::findPathEditControl(nullptr), nullptr);
    EXPECT_EQ(tools3000::dialog::DialogNavigator::findAddressBandControl(nullptr), nullptr);
    EXPECT_EQ(tools3000::dialog::DialogNavigator::findShellViewControl(nullptr), nullptr);
    EXPECT_EQ(tools3000::dialog::DialogNavigator::getCurrentDialogFolder(nullptr), "");
    EXPECT_FALSE(tools3000::dialog::DialogNavigator::instance().navigateToFolder(nullptr, ""));
    EXPECT_FALSE(tools3000::dialog::DialogNavigator::instance().navigateToFolder(nullptr, "C:\\NonExistentPath_12345"));
}

TEST(DialogNavigatorTest, NonFileDialogExclusion) {
    // 验证自身创建的普通窗口绝不会被误判为文件对话框
    HWND hwnd = CreateWindowExW(0, L"STATIC", L"TestDialog", WS_POPUP, 0, 0, 100, 100, nullptr, nullptr, GetModuleHandle(nullptr), nullptr);
    if (hwnd) {
        EXPECT_FALSE(tools3000::dialog::DialogNavigator::isFileDialog(hwnd));
        DestroyWindow(hwnd);
    }
}

TEST(DialogNavigatorTest, PropertySheetExclusionAndRecognition) {
    // 1. 多语言标题排查：资源管理器属性对话框多语言标题后缀排除
    HWND propDlgZh = CreateWindowExW(0, L"#32770", L"test.txt 属性", WS_POPUP, 0, 0, 300, 200, nullptr, nullptr, GetModuleHandle(nullptr), nullptr);
    if (propDlgZh) {
        EXPECT_FALSE(tools3000::dialog::DialogNavigator::isFileDialog(propDlgZh, true));
        DestroyWindow(propDlgZh);
    }

    HWND propDlgEn = CreateWindowExW(0, L"#32770", L"document Properties", WS_POPUP, 0, 0, 300, 200, nullptr, nullptr, GetModuleHandle(nullptr), nullptr);
    if (propDlgEn) {
        EXPECT_FALSE(tools3000::dialog::DialogNavigator::isFileDialog(propDlgEn, true));
        DestroyWindow(propDlgEn);
    }

    HWND propDlgJa = CreateWindowExW(0, L"#32770", L"archive.zip のプロパティ", WS_POPUP, 0, 0, 300, 200, nullptr, nullptr, GetModuleHandle(nullptr), nullptr);
    if (propDlgJa) {
        EXPECT_FALSE(tools3000::dialog::DialogNavigator::isFileDialog(propDlgJa, true));
        DestroyWindow(propDlgJa);
    }

    HWND propDlgDe = CreateWindowExW(0, L"#32770", L"daten.pdf Eigenschaften", WS_POPUP, 0, 0, 300, 200, nullptr, nullptr, GetModuleHandle(nullptr), nullptr);
    if (propDlgDe) {
        EXPECT_FALSE(tools3000::dialog::DialogNavigator::isFileDialog(propDlgDe, true));
        DestroyWindow(propDlgDe);
    }

    HWND propDlgHk = CreateWindowExW(0, L"#32770", L"report.docx 內容", WS_POPUP, 0, 0, 300, 200, nullptr, nullptr, GetModuleHandle(nullptr), nullptr);
    if (propDlgHk) {
        EXPECT_FALSE(tools3000::dialog::DialogNavigator::isFileDialog(propDlgHk, true));
        DestroyWindow(propDlgHk);
    }

    // 2. 带有 ID_APPLY_NOW (0x3021) 按钮的标准属性表窗口绝对排除
    HWND dlgWithApply = CreateWindowExW(0, L"#32770", L"Settings", WS_POPUP, 0, 0, 300, 200, nullptr, nullptr, GetModuleHandle(nullptr), nullptr);
    if (dlgWithApply) {
        HWND applyBtn = CreateWindowExW(0, L"BUTTON", L"Apply", WS_CHILD, 0, 0, 50, 20, dlgWithApply,
                                        reinterpret_cast<HMENU>(static_cast<INT_PTR>(0x3021)), GetModuleHandle(nullptr), nullptr);
        EXPECT_NE(applyBtn, nullptr);
        EXPECT_FALSE(tools3000::dialog::DialogNavigator::isFileDialog(dlgWithApply, true));
        DestroyWindow(dlgWithApply);
    }

    // 3. 普通无文件特征对话框绝对排除
    HWND genericDlg = CreateWindowExW(0, L"#32770", L"About", WS_POPUP, 0, 0, 300, 200, nullptr, nullptr, GetModuleHandle(nullptr), nullptr);
    if (genericDlg) {
        EXPECT_FALSE(tools3000::dialog::DialogNavigator::isFileDialog(genericDlg, true));
        DestroyWindow(genericDlg);
    }

    // 4. 普通输入框/重命名框（包含自定义 Edit 1001 与 OK 按钮）绝不能误判为文件对话框
    HWND inputPromptDlg = CreateWindowExW(0, L"#32770", L"Rename File", WS_POPUP, 0, 0, 350, 150, nullptr, nullptr, GetModuleHandle(nullptr), nullptr);
    if (inputPromptDlg) {
        HWND customEdit = CreateWindowExW(0, L"EDIT", L"newname.txt", WS_CHILD, 0, 0, 200, 20, inputPromptDlg,
                                          reinterpret_cast<HMENU>(static_cast<INT_PTR>(1001)), GetModuleHandle(nullptr), nullptr);
        HWND okBtn = CreateWindowExW(0, L"BUTTON", L"OK", WS_CHILD, 0, 0, 50, 20, inputPromptDlg,
                                     reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDOK)), GetModuleHandle(nullptr), nullptr);
        EXPECT_NE(customEdit, nullptr);
        EXPECT_NE(okBtn, nullptr);
        EXPECT_FALSE(tools3000::dialog::DialogNavigator::isFileDialog(inputPromptDlg, true));
        DestroyWindow(inputPromptDlg);
    }

    // 5. 经典文件对话框特征（标准 edt1: 1152 + IDOK 确认按钮）正向识别
    HWND fileDlg = CreateWindowExW(0, L"#32770", L"Save As", WS_POPUP, 0, 0, 400, 300, nullptr, nullptr, GetModuleHandle(nullptr), nullptr);
    if (fileDlg) {
        HWND edit = CreateWindowExW(0, L"EDIT", L"doc.txt", WS_CHILD, 0, 0, 100, 20, fileDlg,
                                    reinterpret_cast<HMENU>(static_cast<INT_PTR>(1152)), GetModuleHandle(nullptr), nullptr);
        HWND okBtn = CreateWindowExW(0, L"BUTTON", L"Save", WS_CHILD, 0, 0, 50, 20, fileDlg,
                                     reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDOK)), GetModuleHandle(nullptr), nullptr);
        EXPECT_NE(edit, nullptr);
        EXPECT_NE(okBtn, nullptr);
        EXPECT_TRUE(tools3000::dialog::DialogNavigator::isFileDialog(fileDlg, true));
        DestroyWindow(fileDlg);
    }
}

TEST(DialogNavigatorTest, ProgressAndTransferDialogExclusion) {
    auto ensureClassRegistered = [](const wchar_t* className) {
        WNDCLASSEXW wc = {sizeof(WNDCLASSEXW)};
        if (!GetClassInfoExW(GetModuleHandle(nullptr), className, &wc)) {
            wc.lpfnWndProc = DefWindowProcW;
            wc.hInstance = GetModuleHandle(nullptr);
            wc.lpszClassName = className;
            RegisterClassExW(&wc);
        }
    };
    ensureClassRegistered(L"OperationStatusWindow");
    ensureClassRegistered(L"msctls_progress32");

    // 1. OperationStatusWindow 窗口类名（Windows 资源管理器现代文件传输/复制窗口）必须 100% 判定为进度窗口且非文件对话框
    HWND opStatusWnd = CreateWindowExW(0, L"OperationStatusWindow", L"Operation", WS_POPUP, 0, 0, 400, 200, nullptr, nullptr, GetModuleHandle(nullptr), nullptr);
    if (opStatusWnd) {
        EXPECT_TRUE(tools3000::dialog::DialogNavigator::isProgressOrTransferDialog(opStatusWnd));
        EXPECT_FALSE(tools3000::dialog::DialogNavigator::isFileDialog(opStatusWnd, true));
        DestroyWindow(opStatusWnd);
    }

    // 2. 多语言标题匹配：包含“正在复制”、“Copying”等文件传输关键字的 #32770 对话框必须 100% 排除
    HWND copyDlgZh = CreateWindowExW(0, L"#32770", L"正在复制 1,234 个项目 (剩余 30 秒)", WS_POPUP, 0, 0, 400, 200, nullptr, nullptr, GetModuleHandle(nullptr), nullptr);
    if (copyDlgZh) {
        EXPECT_TRUE(tools3000::dialog::DialogNavigator::isProgressOrTransferDialog(copyDlgZh));
        EXPECT_FALSE(tools3000::dialog::DialogNavigator::isFileDialog(copyDlgZh, true));
        DestroyWindow(copyDlgZh);
    }

    HWND copyDlgEn = CreateWindowExW(0, L"#32770", L"Copying 500 items from Source to Target", WS_POPUP, 0, 0, 400, 200, nullptr, nullptr, GetModuleHandle(nullptr), nullptr);
    if (copyDlgEn) {
        EXPECT_TRUE(tools3000::dialog::DialogNavigator::isProgressOrTransferDialog(copyDlgEn));
        EXPECT_FALSE(tools3000::dialog::DialogNavigator::isFileDialog(copyDlgEn, true));
        DestroyWindow(copyDlgEn);
    }

    // 3. 包含 msctls_progress32 进度条控件的普通标题弹窗，必须判定为进度窗口且排除
    HWND progControlDlg = CreateWindowExW(0, L"#32770", L"Transferring Data", WS_POPUP, 0, 0, 400, 200, nullptr, nullptr, GetModuleHandle(nullptr), nullptr);
    if (progControlDlg) {
        HWND progressBar = CreateWindowExW(0, L"msctls_progress32", L"", WS_CHILD, 0, 0, 200, 20, progControlDlg,
                                          reinterpret_cast<HMENU>(static_cast<INT_PTR>(100)), GetModuleHandle(nullptr), nullptr);
        HWND cancelBtn = CreateWindowExW(0, L"BUTTON", L"Cancel", WS_CHILD, 0, 0, 50, 20, progControlDlg,
                                         reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDCANCEL)), GetModuleHandle(nullptr), nullptr);
        EXPECT_NE(progressBar, nullptr);
        EXPECT_NE(cancelBtn, nullptr);
        EXPECT_TRUE(tools3000::dialog::DialogNavigator::isProgressOrTransferDialog(progControlDlg));
        EXPECT_FALSE(tools3000::dialog::DialogNavigator::isFileDialog(progControlDlg, true));
        DestroyWindow(progControlDlg);
    }

    // 4. 只有 IDCANCEL 取消按钮而无 IDOK 确认按钮的 #32770 窗口，绝不能判定为文件对话框
    HWND cancelOnlyDlg = CreateWindowExW(0, L"#32770", L"Working...", WS_POPUP, 0, 0, 400, 200, nullptr, nullptr, GetModuleHandle(nullptr), nullptr);
    if (cancelOnlyDlg) {
        HWND cancelBtn = CreateWindowExW(0, L"BUTTON", L"Cancel", WS_CHILD, 0, 0, 50, 20, cancelOnlyDlg,
                                         reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDCANCEL)), GetModuleHandle(nullptr), nullptr);
        EXPECT_NE(cancelBtn, nullptr);
        EXPECT_FALSE(tools3000::dialog::DialogNavigator::isFileDialog(cancelOnlyDlg, true));
        DestroyWindow(cancelOnlyDlg);
    }
}

TEST(DialogNavigatorTest, InstallerProcessDetection) {
    // 1. 安装/卸载/更新引导程序关键词正向识别
    EXPECT_TRUE(tools3000::dialog::DialogNavigator::isInstallerProcess("setup.exe"));
    EXPECT_TRUE(tools3000::dialog::DialogNavigator::isInstallerProcess("SETUP.EXE"));
    EXPECT_TRUE(tools3000::dialog::DialogNavigator::isInstallerProcess("Git-2.43.0-64-bit.setup.exe"));
    EXPECT_TRUE(tools3000::dialog::DialogNavigator::isInstallerProcess("VSCodeUserSetup-x64-1.85.1.exe"));
    EXPECT_TRUE(tools3000::dialog::DialogNavigator::isInstallerProcess("MyApp_installer.exe"));
    EXPECT_TRUE(tools3000::dialog::DialogNavigator::isInstallerProcess("unins000.exe"));
    EXPECT_TRUE(tools3000::dialog::DialogNavigator::isInstallerProcess("uninstall.exe"));
    EXPECT_TRUE(tools3000::dialog::DialogNavigator::isInstallerProcess("msiexec.exe"));
    EXPECT_TRUE(tools3000::dialog::DialogNavigator::isInstallerProcess("app_update.exe"));
    EXPECT_TRUE(tools3000::dialog::DialogNavigator::isInstallerProcess("patch_installer.exe"));
    EXPECT_TRUE(tools3000::dialog::DialogNavigator::isInstallerProcess("deploy_wizard.exe"));

    // 2. 普通日常软件反向识别（不得误判为安装程序）
    EXPECT_FALSE(tools3000::dialog::DialogNavigator::isInstallerProcess("Code.exe"));
    EXPECT_FALSE(tools3000::dialog::DialogNavigator::isInstallerProcess("notepad.exe"));
    EXPECT_FALSE(tools3000::dialog::DialogNavigator::isInstallerProcess("explorer.exe"));
    EXPECT_FALSE(tools3000::dialog::DialogNavigator::isInstallerProcess("chrome.exe"));
    EXPECT_FALSE(tools3000::dialog::DialogNavigator::isInstallerProcess("photoshop.exe"));
    EXPECT_FALSE(tools3000::dialog::DialogNavigator::isInstallerProcess("powershell.exe"));
    EXPECT_FALSE(tools3000::dialog::DialogNavigator::isInstallerProcess(""));
}

TEST(DialogNavigatorTest, FolderPickerRecognition) {
    auto ensureClassRegistered = [](const wchar_t* className) {
        WNDCLASSEXW wc = {sizeof(WNDCLASSEXW)};
        if (!GetClassInfoExW(GetModuleHandle(nullptr), className, &wc)) {
            wc.lpfnWndProc = DefWindowProcW;
            wc.hInstance = GetModuleHandle(nullptr);
            wc.lpszClassName = className;
            RegisterClassExW(&wc);
        }
    };
    ensureClassRegistered(L"SysTreeView32");
    ensureClassRegistered(L"NamespaceTreeControl");

    // 1. 标准 SHBrowseForFolder 树形控件 + 确认按钮 + 取消按钮
    HWND pickerDlg = CreateWindowExW(0, L"#32770", L"选择文件夹", WS_POPUP, 0, 0, 350, 400, nullptr, nullptr, GetModuleHandle(nullptr), nullptr);
    if (pickerDlg) {
        HWND tree = CreateWindowExW(0, L"SysTreeView32", L"", WS_CHILD, 0, 0, 300, 300, pickerDlg,
                                   reinterpret_cast<HMENU>(static_cast<INT_PTR>(100)), GetModuleHandle(nullptr), nullptr);
        HWND okBtn = CreateWindowExW(0, L"BUTTON", L"确定", WS_CHILD, 0, 0, 60, 25, pickerDlg,
                                     reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDOK)), GetModuleHandle(nullptr), nullptr);
        HWND cancelBtn = CreateWindowExW(0, L"BUTTON", L"取消", WS_CHILD, 0, 0, 60, 25, pickerDlg,
                                         reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDCANCEL)), GetModuleHandle(nullptr), nullptr);
        EXPECT_NE(tree, nullptr);
        EXPECT_NE(okBtn, nullptr);
        EXPECT_NE(cancelBtn, nullptr);

        EXPECT_FALSE(tools3000::dialog::DialogNavigator::isProgressOrTransferDialog(pickerDlg));
        EXPECT_TRUE(tools3000::dialog::DialogNavigator::isFileDialog(pickerDlg, true));
        EXPECT_EQ(tools3000::dialog::DialogNavigator::detectDialogType(pickerDlg), tools3000::dialog::DialogType::FolderPicker);

        DestroyWindow(pickerDlg);
    }

    // 2. 现代 NamespaceTreeControl 架构
    HWND nsPickerDlg = CreateWindowExW(0, L"#32770", L"Browse For Folder", WS_POPUP, 0, 0, 350, 400, nullptr, nullptr, GetModuleHandle(nullptr), nullptr);
    if (nsPickerDlg) {
        HWND nsTree = CreateWindowExW(0, L"NamespaceTreeControl", L"", WS_CHILD, 0, 0, 300, 300, nsPickerDlg,
                                     reinterpret_cast<HMENU>(static_cast<INT_PTR>(101)), GetModuleHandle(nullptr), nullptr);
        HWND okBtn = CreateWindowExW(0, L"BUTTON", L"OK", WS_CHILD, 0, 0, 60, 25, nsPickerDlg,
                                     reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDOK)), GetModuleHandle(nullptr), nullptr);
        HWND cancelBtn = CreateWindowExW(0, L"BUTTON", L"Cancel", WS_CHILD, 0, 0, 60, 25, nsPickerDlg,
                                         reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDCANCEL)), GetModuleHandle(nullptr), nullptr);
        EXPECT_NE(nsTree, nullptr);
        EXPECT_NE(okBtn, nullptr);
        EXPECT_NE(cancelBtn, nullptr);

        EXPECT_FALSE(tools3000::dialog::DialogNavigator::isProgressOrTransferDialog(nsPickerDlg));
        EXPECT_TRUE(tools3000::dialog::DialogNavigator::isFileDialog(nsPickerDlg, true));
        EXPECT_EQ(tools3000::dialog::DialogNavigator::detectDialogType(nsPickerDlg), tools3000::dialog::DialogType::FolderPicker);

        DestroyWindow(nsPickerDlg);
    }
}

TEST(DialogNavigatorTest, InstallerOrWizardDetection) {
    // 1. 窗口标题命中向导关键字（非标准进程名如 A.exe）
    HWND wizardDlg = CreateWindowExW(0, L"#32770", L"Software A 安装向导 (Setup Wizard)", WS_POPUP, 0, 0, 500, 400, nullptr, nullptr, GetModuleHandle(nullptr), nullptr);
    if (wizardDlg) {
        EXPECT_TRUE(tools3000::dialog::DialogNavigator::isInstallerOrWizard(wizardDlg));
        DestroyWindow(wizardDlg);
    }

    // 2. 包含向导后退按钮 (< Back / 上一步 / ID 0x3023)
    HWND customWizard = CreateWindowExW(0, L"#32770", L"Select Options", WS_POPUP, 0, 0, 500, 400, nullptr, nullptr, GetModuleHandle(nullptr), nullptr);
    if (customWizard) {
        HWND backBtn = CreateWindowExW(0, L"BUTTON", L"< 上一步(&B)", WS_CHILD, 0, 0, 60, 25, customWizard,
                                      reinterpret_cast<HMENU>(static_cast<INT_PTR>(0x3023)), GetModuleHandle(nullptr), nullptr);
        EXPECT_NE(backBtn, nullptr);
        EXPECT_TRUE(tools3000::dialog::DialogNavigator::isInstallerOrWizard(customWizard));
        DestroyWindow(customWizard);
    }

    // 3. 普通文件对话框（非向导）应返回 false
    HWND normalDlg = CreateWindowExW(0, L"#32770", L"另存为", WS_POPUP, 0, 0, 400, 300, nullptr, nullptr, GetModuleHandle(nullptr), nullptr);
    if (normalDlg) {
        EXPECT_FALSE(tools3000::dialog::DialogNavigator::isInstallerOrWizard(normalDlg));
        DestroyWindow(normalDlg);
    }
}

TEST(DialogNavigatorTest, WizardComponentTreeExclusion) {
    auto ensureClassRegistered = [](const wchar_t* className) {
        WNDCLASSEXW wc = {sizeof(WNDCLASSEXW)};
        if (!GetClassInfoExW(GetModuleHandle(nullptr), className, &wc)) {
            wc.lpfnWndProc = DefWindowProcW;
            wc.hInstance = GetModuleHandle(nullptr);
            wc.lpszClassName = className;
            RegisterClassExW(&wc);
        }
    };
    ensureClassRegistered(L"SysTreeView32");

    // 安装程序“选择组件”向导页（包含树形控件、Next 按钮与 Back 按钮，总控件数 > 18）
    // 绝不能误判为目录选择器或文件对话框
    HWND compPage = CreateWindowExW(0, L"#32770", L"选择安装组件", WS_POPUP, 0, 0, 550, 450, nullptr, nullptr, GetModuleHandle(nullptr), nullptr);
    if (compPage) {
        HWND tree = CreateWindowExW(0, L"SysTreeView32", L"", WS_CHILD, 0, 0, 300, 200, compPage,
                                   reinterpret_cast<HMENU>(static_cast<INT_PTR>(100)), GetModuleHandle(nullptr), nullptr);
        HWND nextBtn = CreateWindowExW(0, L"BUTTON", L"下一步(&N) >", WS_CHILD, 0, 0, 70, 25, compPage,
                                      reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDOK)), GetModuleHandle(nullptr), nullptr);
        HWND backBtn = CreateWindowExW(0, L"BUTTON", L"< 上一步(&B)", WS_CHILD, 0, 0, 70, 25, compPage,
                                      reinterpret_cast<HMENU>(static_cast<INT_PTR>(0x3023)), GetModuleHandle(nullptr), nullptr);
        HWND cancelBtn = CreateWindowExW(0, L"BUTTON", L"取消", WS_CHILD, 0, 0, 70, 25, compPage,
                                         reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDCANCEL)), GetModuleHandle(nullptr), nullptr);
        EXPECT_NE(tree, nullptr);
        EXPECT_NE(nextBtn, nullptr);
        EXPECT_NE(backBtn, nullptr);
        EXPECT_NE(cancelBtn, nullptr);

        EXPECT_TRUE(tools3000::dialog::DialogNavigator::isInstallerOrWizard(compPage));
        EXPECT_FALSE(tools3000::dialog::DialogNavigator::isFileDialog(compPage, true));
        EXPECT_EQ(tools3000::dialog::DialogNavigator::detectDialogType(compPage), tools3000::dialog::DialogType::Unknown);

        DestroyWindow(compPage);
    }
}

TEST(DialogNavigatorTest, FolderPickerSafeNavigation) {
    auto ensureClassRegistered = [](const wchar_t* className) {
        WNDCLASSEXW wc = {sizeof(WNDCLASSEXW)};
        if (!GetClassInfoExW(GetModuleHandle(nullptr), className, &wc)) {
            wc.lpfnWndProc = DefWindowProcW;
            wc.hInstance = GetModuleHandle(nullptr);
            wc.lpszClassName = className;
            RegisterClassExW(&wc);
        }
    };
    ensureClassRegistered(L"SysTreeView32");

    HWND pickerDlg = CreateWindowExW(0, L"#32770", L"选择文件夹", WS_POPUP, 0, 0, 400, 400, nullptr, nullptr, GetModuleHandle(nullptr), nullptr);
    if (pickerDlg) {
        HWND tree = CreateWindowExW(0, L"SysTreeView32", L"", WS_CHILD, 0, 0, 300, 250, pickerDlg,
                                   reinterpret_cast<HMENU>(static_cast<INT_PTR>(100)), GetModuleHandle(nullptr), nullptr);
        HWND edit = CreateWindowExW(0, L"EDIT", L"C:\\OldFolder", WS_CHILD, 0, 0, 300, 25, pickerDlg,
                                   reinterpret_cast<HMENU>(static_cast<INT_PTR>(1001)), GetModuleHandle(nullptr), nullptr);
        HWND okBtn = CreateWindowExW(0, L"BUTTON", L"确定", WS_CHILD, 0, 0, 60, 25, pickerDlg,
                                     reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDOK)), GetModuleHandle(nullptr), nullptr);
        HWND cancelBtn = CreateWindowExW(0, L"BUTTON", L"取消", WS_CHILD, 0, 0, 60, 25, pickerDlg,
                                         reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDCANCEL)), GetModuleHandle(nullptr), nullptr);
        EXPECT_NE(tree, nullptr);
        EXPECT_NE(edit, nullptr);
        EXPECT_NE(okBtn, nullptr);
        EXPECT_NE(cancelBtn, nullptr);

        EXPECT_TRUE(tools3000::dialog::DialogNavigator::isFileDialog(pickerDlg, true));
        EXPECT_EQ(tools3000::dialog::DialogNavigator::detectDialogType(pickerDlg), tools3000::dialog::DialogType::FolderPicker);

        std::string tempDir = tools3000::core::WinUtils::wstringToUtf8(std::filesystem::temp_directory_path().native());
        bool navigated = tools3000::dialog::DialogNavigator::instance().navigateToFolder(pickerDlg, tempDir);
        EXPECT_TRUE(navigated);

        // 验证文本已被安全更新为目标目录，且没有被恢复为 OldFolder，也没有导致窗口意外关闭
        EXPECT_TRUE(IsWindow(pickerDlg));
        wchar_t textBuf[MAX_PATH] = {0};
        GetWindowTextW(edit, textBuf, MAX_PATH);
        std::wstring currentText = textBuf;
        EXPECT_NE(currentText, L"C:\\OldFolder");
        EXPECT_EQ(currentText, std::filesystem::temp_directory_path().native());

        DestroyWindow(pickerDlg);
    }
}

// -----------------------------------------------------------------------------
// 56. 选区圆角手柄交互与实时调节测试套件
// -----------------------------------------------------------------------------