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

    // 5. 现代 IFileDialog (含 SHELLDLL_DefView 及 Address Band Root 内嵌 msctls_progress32) 绝不能被误判为传输进度框
    ensureClassRegistered(L"SHELLDLL_DefView");
    ensureClassRegistered(L"Address Band Root");
    HWND modernFileDlg = CreateWindowExW(0, L"#32770", L"打开文件夹", WS_POPUP, 0, 0, 600, 450, nullptr, nullptr, GetModuleHandle(nullptr), nullptr);
    if (modernFileDlg) {
        HWND defView = CreateWindowExW(0, L"SHELLDLL_DefView", L"", WS_CHILD, 0, 40, 580, 350, modernFileDlg,
                                       reinterpret_cast<HMENU>(static_cast<INT_PTR>(100)), GetModuleHandle(nullptr), nullptr);
        HWND addrBand = CreateWindowExW(0, L"Address Band Root", L"", WS_CHILD, 0, 0, 580, 32, modernFileDlg,
                                        reinterpret_cast<HMENU>(static_cast<INT_PTR>(41477)), GetModuleHandle(nullptr), nullptr);
        HWND addrProg = CreateWindowExW(0, L"msctls_progress32", L"", WS_CHILD, 0, 0, 580, 32, addrBand,
                                        reinterpret_cast<HMENU>(static_cast<INT_PTR>(101)), GetModuleHandle(nullptr), nullptr);
        HWND okBtn = CreateWindowExW(0, L"BUTTON", L"选择文件夹", WS_CHILD, 420, 400, 80, 28, modernFileDlg,
                                     reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDOK)), GetModuleHandle(nullptr), nullptr);
        EXPECT_NE(defView, nullptr);
        EXPECT_NE(addrBand, nullptr);
        EXPECT_NE(addrProg, nullptr);
        EXPECT_NE(okBtn, nullptr);
        EXPECT_FALSE(tools3000::dialog::DialogNavigator::isProgressOrTransferDialog(modernFileDlg));
        EXPECT_TRUE(tools3000::dialog::DialogNavigator::isFileDialog(modernFileDlg, true));
        EXPECT_EQ(tools3000::dialog::DialogNavigator::detectDialogType(modernFileDlg), tools3000::dialog::DialogType::Modern);
        DestroyWindow(modernFileDlg);
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

#include "dialog/DialogEventAndLayoutPolicy.h"

TEST(DialogEventAndLayoutPolicyTest, WinEventFilterPreservesChildInvokedAndBlocksChildHide) {
    const HWND fakeHwnd = reinterpret_cast<HWND>(static_cast<uintptr_t>(0x12340001));
    EXPECT_FALSE(tools3000::dialog::shouldProcessDialogWinEvent(EVENT_OBJECT_INVOKED, nullptr, OBJID_WINDOW, CHILDID_SELF));

    EXPECT_TRUE(tools3000::dialog::shouldProcessDialogWinEvent(EVENT_OBJECT_INVOKED, fakeHwnd, OBJID_WINDOW, CHILDID_SELF));
    EXPECT_TRUE(tools3000::dialog::shouldProcessDialogWinEvent(EVENT_OBJECT_INVOKED, fakeHwnd, OBJID_CLIENT, CHILDID_SELF));
    EXPECT_TRUE(tools3000::dialog::shouldProcessDialogWinEvent(EVENT_OBJECT_INVOKED, fakeHwnd, 5, 0));
    EXPECT_TRUE(tools3000::dialog::shouldProcessDialogWinEvent(EVENT_OBJECT_INVOKED, fakeHwnd, OBJID_CLIENT, 3));

    EXPECT_TRUE(tools3000::dialog::shouldProcessDialogWinEvent(EVENT_OBJECT_HIDE, fakeHwnd, OBJID_WINDOW, CHILDID_SELF));
    EXPECT_FALSE(tools3000::dialog::shouldProcessDialogWinEvent(EVENT_OBJECT_HIDE, fakeHwnd, 4, CHILDID_SELF));
    EXPECT_FALSE(tools3000::dialog::shouldProcessDialogWinEvent(EVENT_OBJECT_HIDE, fakeHwnd, OBJID_WINDOW, 2));
    EXPECT_FALSE(tools3000::dialog::shouldProcessDialogWinEvent(EVENT_OBJECT_DESTROY, fakeHwnd, 1, 1));
    EXPECT_FALSE(tools3000::dialog::shouldProcessDialogWinEvent(EVENT_OBJECT_SHOW, fakeHwnd, 2, CHILDID_SELF));
}

TEST(DialogEventAndLayoutPolicyTest, RibbonSafeHorizontalPlacementProtectsCaptionButtons) {
    for (float scale : {1.0f, 1.5f}) {
        const int leftSafeOffset = static_cast<int>(112.0f * scale);
        const int rightSafeOffset = static_cast<int>(145.0f * scale);

        RECT wideDlg{100, 100, 100 + static_cast<int>(800 * scale), 600};
        int posX = 0;
        int maxAvail = 0;
        const int wideRibbonW = static_cast<int>(320 * scale);
        EXPECT_TRUE(tools3000::dialog::computeSafeRibbonHorizontalPlacement(
            wideDlg, scale, wideRibbonW, "top-right", posX, maxAvail));
        EXPECT_GE(posX, wideDlg.left + leftSafeOffset);
        EXPECT_LE(posX + wideRibbonW, wideDlg.right - rightSafeOffset);

        EXPECT_TRUE(tools3000::dialog::computeSafeRibbonHorizontalPlacement(
            wideDlg, scale, wideRibbonW, "top-center", posX, maxAvail));
        EXPECT_GE(posX, wideDlg.left + leftSafeOffset);
        EXPECT_LE(posX + wideRibbonW, wideDlg.right - rightSafeOffset);

        RECT compactDlg{200, 150, 200 + static_cast<int>(360 * scale), 500};
        const int compactRibbonW = static_cast<int>(90 * scale);
        EXPECT_TRUE(tools3000::dialog::computeSafeRibbonHorizontalPlacement(
            compactDlg, scale, compactRibbonW, "top-center", posX, maxAvail));
        EXPECT_GE(posX, compactDlg.left + leftSafeOffset);
        EXPECT_LE(posX + compactRibbonW, compactDlg.right - rightSafeOffset);

        EXPECT_FALSE(tools3000::dialog::computeSafeRibbonHorizontalPlacement(
            compactDlg, scale, wideRibbonW, "top-right", posX, maxAvail));

        RECT tinyDlg{0, 0, static_cast<int>(280 * scale), 300};
        EXPECT_FALSE(tools3000::dialog::computeSafeRibbonHorizontalPlacement(
            tinyDlg, scale, static_cast<int>(64 * scale), "top-right", posX, maxAvail));
    }
}

TEST(DialogEventAndLayoutPolicyTest, RibbonRightAlignedRelayoutShiftsLeftWhenCapsuleExpands) {
    for (float scale : {1.0f, 1.5f, 2.0f}) {
        const int rightSafeOffset = static_cast<int>(145.0f * scale);
        RECT dlgRect{120, 80, 120 + static_cast<int>(860 * scale), 640};

        int initialPosX = 0;
        int maxAvail = 0;
        const int narrowW = static_cast<int>(160 * scale);
        ASSERT_TRUE(tools3000::dialog::computeSafeRibbonHorizontalPlacement(
            dlgRect, scale, narrowW, "top-right", initialPosX, maxAvail));

        int expandedPosX = 0;
        const int expandedW = static_cast<int>(340 * scale);
        ASSERT_TRUE(tools3000::dialog::computeSafeRibbonHorizontalPlacement(
            dlgRect, scale, expandedW, "top-right", expandedPosX, maxAvail));

        EXPECT_LT(expandedPosX, initialPosX);
        EXPECT_LE(expandedPosX + expandedW, dlgRect.right - rightSafeOffset);
    }
}

#include "core/ipc/SearchPipeClient.h"

TEST(DialogClassificationTest, FiveArchitecturesAndThreeModes) {
    using tools3000::dialog::DialogArchitecture;
    using tools3000::dialog::DialogMode;
    using tools3000::dialog::DialogModeSignals;
    using tools3000::dialog::DialogStructureSignals;
    using tools3000::dialog::classifyDialogArchitecture;
    using tools3000::dialog::classifyDialogMode;

    // 1. MECE 5 类对话框架构分类
    {
        DialogStructureSignals modern{};
        modern.hasDefView = true;
        modern.hasDirectUI = true;
        modern.hasAddressBand = true;
        modern.hasOkButton = true;
        EXPECT_EQ(classifyDialogArchitecture(modern), DialogArchitecture::ModernIFileDialog);
    }
    {
        // OFN_EXPLORER: 含 SHELLDLL_DefView 但无 DirectUIHWND / Address Band
        DialogStructureSignals ofnExplorer{};
        ofnExplorer.hasDefView = true;
        ofnExplorer.hasDirectUI = false;
        ofnExplorer.hasAddressBand = false;
        ofnExplorer.hasFileNameCombo1148 = true;
        ofnExplorer.hasOkButton = true;
        EXPECT_EQ(classifyDialogArchitecture(ofnExplorer), DialogArchitecture::LegacyExplorer);
    }
    {
        // Classic OPENFILENAME: 仅 edt1 (1152) / cmb13 (1148)
        DialogStructureSignals classic{};
        classic.hasFileNameEdit1152 = true;
        classic.hasOkButton = true;
        EXPECT_EQ(classifyDialogArchitecture(classic), DialogArchitecture::ClassicOpenFileName);
    }
    {
        // BrowseForFolder: 树形控件 + OK + Cancel + 控件总数 <= 18
        DialogStructureSignals browse{};
        browse.hasTreeView = true;
        browse.hasOkButton = true;
        browse.hasCancelButton = true;
        browse.totalControls = 9;
        EXPECT_EQ(classifyDialogArchitecture(browse), DialogArchitecture::BrowseForFolder);
    }
    {
        // Unknown: 属性表 / 向导 / 进度弹窗一律安全排除
        DialogStructureSignals propSheet{};
        propSheet.hasDefView = true;
        propSheet.hasDirectUI = true;
        propSheet.hasOkButton = true;
        propSheet.hasApplyButton = true;
        EXPECT_EQ(classifyDialogArchitecture(propSheet), DialogArchitecture::Unknown);

        DialogStructureSignals wizard{};
        wizard.hasTreeView = true;
        wizard.hasOkButton = true;
        wizard.hasCancelButton = true;
        wizard.hasBackButton = true;
        wizard.totalControls = 8;
        EXPECT_EQ(classifyDialogArchitecture(wizard), DialogArchitecture::Unknown);
    }

    // 2. 3 种业务模式分类（含 PickFolder 结构指纹、8 语种词表与 SaveFile 保守安全边界）
    {
        // Modern PickFolder: DirectUI + DefView 且无 ComboBox 1136
        DialogModeSignals pick{};
        pick.architecture = DialogArchitecture::ModernIFileDialog;
        pick.hasDirectUI = true;
        pick.hasDefView = true;
        pick.hasFileTypeCombo1136 = false;
        pick.okButtonText = L"选择文件夹(&S)";
        EXPECT_EQ(classifyDialogMode(pick), DialogMode::PickFolder);
    }
    {
        // Modern OpenFile: 有 ComboBox 1136 + 打开按钮
        DialogModeSignals openDlg{};
        openDlg.architecture = DialogArchitecture::ModernIFileDialog;
        openDlg.hasDirectUI = true;
        openDlg.hasDefView = true;
        openDlg.hasFileTypeCombo1136 = true;
        openDlg.okButtonText = L"&Open";
        openDlg.systemOpenButtonText = L"&Open";
        openDlg.systemSaveButtonText = L"&Save";
        openDlg.dialogTitle = L"Open File";
        EXPECT_EQ(classifyDialogMode(openDlg), DialogMode::OpenFile);
    }
    {
        // Modern SaveFile: 另存为标题保护 + Save 按钮（8 语种）
        for (const wchar_t* saveBtn : {L"保存(&S)", L"儲存(&S)", L"&Save", L"保存", L"저장(&S)",
                                       L"Сохранить", L"Salvar", L"Guardar"}) {
            DialogModeSignals saveDlg{};
            saveDlg.architecture = DialogArchitecture::ModernIFileDialog;
            saveDlg.hasDirectUI = true;
            saveDlg.hasDefView = true;
            saveDlg.hasFileTypeCombo1136 = true;
            saveDlg.okButtonText = saveBtn;
            EXPECT_EQ(classifyDialogMode(saveDlg), DialogMode::SaveFile);
        }
    }
    {
        // 防御测试：即便系统按钮与 Open 同词，但标题为“另存为”也必须归类 SaveFile
        DialogModeSignals guarded{};
        guarded.architecture = DialogArchitecture::ModernIFileDialog;
        guarded.hasDirectUI = true;
        guarded.hasDefView = true;
        guarded.hasFileTypeCombo1136 = true;
        guarded.okButtonText = L"&Open";
        guarded.systemOpenButtonText = L"&Open";
        guarded.dialogTitle = L"另存为";
        EXPECT_EQ(classifyDialogMode(guarded), DialogMode::SaveFile);
    }
    {
        // 无法明确判断时：有预填文件名保守判 SaveFile，无预填判 Unknown（两者均严禁改写文件名）
        DialogModeSignals prefilledNeutral{};
        prefilledNeutral.architecture = DialogArchitecture::ClassicOpenFileName;
        prefilledNeutral.okButtonText = L"OK";
        prefilledNeutral.dialogTitle = L"Select Target";
        prefilledNeutral.fileNameEditHasInitialText = true;
        EXPECT_EQ(classifyDialogMode(prefilledNeutral), DialogMode::SaveFile);

        DialogModeSignals emptyNeutral = prefilledNeutral;
        emptyNeutral.fileNameEditHasInitialText = false;
        EXPECT_EQ(classifyDialogMode(emptyNeutral), DialogMode::Unknown);
    }

    // 3. 真实 Win32 层级验证：OFN_EXPLORER (LegacyExplorer) 与 ComboBoxEx32 -> ComboBox -> Edit 穿透
    auto ensureClassRegistered = [](const wchar_t* className) {
        WNDCLASSEXW wc{sizeof(WNDCLASSEXW)};
        if (!GetClassInfoExW(GetModuleHandleW(nullptr), className, &wc)) {
            wc.lpfnWndProc = DefWindowProcW;
            wc.hInstance = GetModuleHandleW(nullptr);
            wc.lpszClassName = className;
            RegisterClassExW(&wc);
        }
    };
    ensureClassRegistered(L"SHELLDLL_DefView");
    ensureClassRegistered(L"ComboBoxEx32");

    HWND legacyDlg = CreateWindowExW(0, L"#32770", L"打开文件", WS_POPUP, 0, 0, 500, 360,
                                     nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    ASSERT_NE(legacyDlg, nullptr);
    HWND defView = CreateWindowExW(0, L"SHELLDLL_DefView", L"", WS_CHILD | WS_VISIBLE, 0, 20, 480, 240,
                                   legacyDlg, reinterpret_cast<HMENU>(static_cast<INT_PTR>(100)),
                                   GetModuleHandleW(nullptr), nullptr);
    HWND comboEx = CreateWindowExW(0, L"ComboBoxEx32", L"", WS_CHILD | WS_VISIBLE, 10, 280, 320, 24,
                                   legacyDlg, reinterpret_cast<HMENU>(static_cast<INT_PTR>(1148)),
                                   GetModuleHandleW(nullptr), nullptr);
    HWND innerCombo = CreateWindowExW(0, L"COMBOBOX", L"", WS_CHILD | WS_VISIBLE | CBS_DROPDOWN, 0, 0, 320, 24,
                                      comboEx, reinterpret_cast<HMENU>(static_cast<INT_PTR>(1000)),
                                      GetModuleHandleW(nullptr), nullptr);
    HWND nestedEdit = CreateWindowExW(0, L"EDIT", L"legacy.txt", WS_CHILD | WS_VISIBLE, 0, 0, 300, 20,
                                      innerCombo, reinterpret_cast<HMENU>(static_cast<INT_PTR>(1001)),
                                      GetModuleHandleW(nullptr), nullptr);
    HWND okBtn = CreateWindowExW(0, L"BUTTON", L"打开(&O)", WS_CHILD | WS_VISIBLE, 360, 280, 75, 24,
                                 legacyDlg, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDOK)),
                                 GetModuleHandleW(nullptr), nullptr);
    EXPECT_NE(defView, nullptr);
    EXPECT_NE(nestedEdit, nullptr);
    EXPECT_NE(okBtn, nullptr);

    EXPECT_EQ(tools3000::dialog::DialogNavigator::detectDialogArchitecture(legacyDlg, true),
              DialogArchitecture::LegacyExplorer);
    EXPECT_EQ(tools3000::dialog::DialogNavigator::detectDialogType(legacyDlg),
              tools3000::dialog::DialogType::Legacy);
    EXPECT_EQ(tools3000::dialog::DialogNavigator::detectDialogMode(legacyDlg, true),
              DialogMode::OpenFile);
    HWND resolvedEdit = tools3000::dialog::DialogNavigator::findPathEditControl(legacyDlg);
    EXPECT_NE(resolvedEdit, nullptr);
    // 确认穿透拿到的是位于 ComboBoxEx32 -> ComboBox 内部的真实 Edit
    wchar_t resolvedCls[32]{};
    GetClassNameW(resolvedEdit, resolvedCls, static_cast<int>(std::size(resolvedCls)));
    EXPECT_STREQ(resolvedCls, L"Edit");

    DestroyWindow(legacyDlg);
}

TEST(DialogInlineSearchSafetyTest, SaveAsAndUnknownNeverOverwriteOrCommit) {
    using tools3000::dialog::DialogMode;
    using tools3000::dialog::SearchActionKind;
    using tools3000::dialog::decideInlineSearchAction;

    // 1. SaveFile 选中文件：无论 Enter / Shift+Enter / Ctrl+Enter，100% 强制降级为仅跳父目录！
    for (bool ctrl : {false, true}) {
        for (bool shift : {false, true}) {
            const auto d = decideInlineSearchAction(DialogMode::SaveFile, /*isDir=*/false, ctrl, shift);
            EXPECT_EQ(d.kind, SearchActionKind::NavigateFolderOnly);
            EXPECT_TRUE(d.useParentDirectory);
        }
    }

    // 2. PickFolder 与 Unknown 选中文件：同样 100% 强制降级为仅跳父目录！
    for (DialogMode safeMode : {DialogMode::PickFolder, DialogMode::Unknown}) {
        const auto d = decideInlineSearchAction(safeMode, /*isDir=*/false, false, true);
        EXPECT_EQ(d.kind, SearchActionKind::NavigateFolderOnly);
        EXPECT_TRUE(d.useParentDirectory);
    }

    // 3. OpenFile 选中文件：Enter 仅填入文件名不提交；Shift+Enter 填入并确认打开；Ctrl+Enter 仅跳父目录
    {
        const auto enterFile = decideInlineSearchAction(DialogMode::OpenFile, false, false, false);
        EXPECT_EQ(enterFile.kind, SearchActionKind::NavigateAndFillFileName);
        EXPECT_TRUE(enterFile.useParentDirectory);

        const auto shiftEnterFile = decideInlineSearchAction(DialogMode::OpenFile, false, false, true);
        EXPECT_EQ(shiftEnterFile.kind, SearchActionKind::NavigateFillAndConfirmOpen);
        EXPECT_TRUE(shiftEnterFile.useParentDirectory);

        const auto ctrlEnterFile = decideInlineSearchAction(DialogMode::OpenFile, false, true, false);
        EXPECT_EQ(ctrlEnterFile.kind, SearchActionKind::NavigateFolderOnly);
        EXPECT_TRUE(ctrlEnterFile.useParentDirectory);
    }

    // 4. 任意模式选中目录：永远只跳目录本身
    for (DialogMode m : {DialogMode::OpenFile, DialogMode::SaveFile, DialogMode::PickFolder, DialogMode::Unknown}) {
        const auto d = decideInlineSearchAction(m, /*isDir=*/true, false, true);
        EXPECT_EQ(d.kind, SearchActionKind::NavigateFolderOnly);
        EXPECT_FALSE(d.useParentDirectory);
    }
}

TEST(DialogEventAndLayoutPolicyTest, DpiUnawareMixedMonitorAndBottomDockPlacement) {
    // 1. DPI-Unaware 老程序在高分屏下使用 max(windowScale, monitorScale) 防止胶囊过小
    EXPECT_FLOAT_EQ(tools3000::dialog::computeEffectiveDialogDpiScale(1.0f, 1.5f), 1.5f);
    EXPECT_FLOAT_EQ(tools3000::dialog::computeEffectiveDialogDpiScale(1.0f, 2.0f), 2.0f);
    EXPECT_FLOAT_EQ(tools3000::dialog::computeEffectiveDialogDpiScale(1.75f, 1.25f), 1.75f);

    // 2. 底部停靠模式基于 DWM 可见边框计算，空间充裕时置于下缘外侧，贴近任务栏底边时自动内收不溢出
    const RECT frameRect{100, 120, 700, 560};
    const RECT normalWork{0, 0, 1920, 1040};
    int posX = 0;
    int posY = 0;
    int maxAvail = 0;
    EXPECT_TRUE(tools3000::dialog::computeSafeRibbonPlacement(
        frameRect, normalWork, 1.5f, 360, 38, "bottom-right", posX, posY, maxAvail));
    EXPECT_GT(posY, frameRect.bottom);
    EXPECT_LE(posY + 38, normalWork.bottom);
    EXPECT_GT(maxAvail, 500);  // 底部停靠不扣标题栏 257px 按钮死区

    // 当对话框底边紧贴屏幕工作区下缘时，自动内收至工作区内
    const RECT tightWork{0, 0, 1920, 565};
    EXPECT_TRUE(tools3000::dialog::computeSafeRibbonPlacement(
        frameRect, tightWork, 1.0f, 320, 25, "bottom-center", posX, posY, maxAvail));
    EXPECT_LE(posY + 25, tightWork.bottom);
    EXPECT_GE(posY, tightWork.top);
}

TEST(DialogInlineSearchPipePolicyTest, QueryBuilderAndDemandStartGuard) {
    using tools3000::core::ipc::SearchPipeClient;
    using tools3000::core::ipc::SearchPipeStatus;

    // 1. folder: 过滤语法自动拼接与幂等去重
    EXPECT_EQ(SearchPipeClient::buildEffectiveQuery("   ", true), "");
    EXPECT_EQ(SearchPipeClient::buildEffectiveQuery("Tools3000", false), "Tools3000");
    EXPECT_EQ(SearchPipeClient::buildEffectiveQuery("Tools3000", true), "folder: Tools3000");
    EXPECT_EQ(SearchPipeClient::buildEffectiveQuery("folder: Tools3000", true), "folder: Tools3000");
    EXPECT_EQ(SearchPipeClient::buildEffectiveQuery("dir:docs", true), "dir:docs");
    EXPECT_EQ(SearchPipeClient::buildEffectiveQuery("file:readme.md", true), "file:readme.md");

    // 2. DEMAND_START 硬守卫：空白输入、非显式打字、autoStart=false 一律禁止拉起搜索服务
    EXPECT_FALSE(SearchPipeClient::shouldAllowAutoStart("", true, true));
    EXPECT_FALSE(SearchPipeClient::shouldAllowAutoStart("   \t ", true, true));
    EXPECT_FALSE(SearchPipeClient::shouldAllowAutoStart("abc", false, true));
    EXPECT_FALSE(SearchPipeClient::shouldAllowAutoStart("abc", true, false));
    EXPECT_TRUE(SearchPipeClient::shouldAllowAutoStart("abc", true, true));

    // 3. autoStart=false 零副作用调用验证：服务未运行时绝不触发拉起且毫秒级返回
    const bool runningBefore = SearchPipeClient::instance().isServiceRunningNow(1);
    const auto resp = SearchPipeClient::instance().queryFiles(
        "tools3000_unit_probe", 999, 5, true, {}, /*autoStart=*/false, 150);
    if (!runningBefore) {
        EXPECT_EQ(resp.status, SearchPipeStatus::ServiceNotRunning);
        EXPECT_FALSE(SearchPipeClient::instance().isWarmupInFlight());
    }
}

TEST(PathMemoryManagerTest, InlineSearchAndBottomPositionPersistence) {
    auto& mgr = tools3000::dialog::PathMemoryManager::instance();
    const bool prevInline = mgr.isInlineSearchEnabled();
    const std::string prevPos = mgr.getRibbonPosition();

    mgr.setInlineSearchEnabled(false);
    EXPECT_FALSE(mgr.isInlineSearchEnabled());
    mgr.setInlineSearchEnabled(true);
    EXPECT_TRUE(mgr.isInlineSearchEnabled());

    mgr.setRibbonPosition("bottom-right");
    EXPECT_EQ(mgr.getRibbonPosition(), "bottom-right");
    mgr.setRibbonPosition("bottom-center");
    EXPECT_EQ(mgr.getRibbonPosition(), "bottom-center");
    mgr.setRibbonPosition("invalid-position");
    EXPECT_EQ(mgr.getRibbonPosition(), "top-right");

    mgr.setInlineSearchEnabled(prevInline);
    mgr.setRibbonPosition(prevPos);
}

TEST(DialogNavigatorTest, AllowsSelfProcessFileDialogWhenEnabled) {
    auto ensureClassRegistered = [](const wchar_t* className) {
        WNDCLASSEXW wc = {sizeof(WNDCLASSEXW)};
        if (!GetClassInfoExW(GetModuleHandle(nullptr), className, &wc)) {
            wc.lpfnWndProc = DefWindowProcW;
            wc.hInstance = GetModuleHandle(nullptr);
            wc.lpszClassName = className;
            RegisterClassExW(&wc);
        }
    };
    ensureClassRegistered(L"SHELLDLL_DefView");
    ensureClassRegistered(L"Address Band Root");

    HWND selfFileDlg = CreateWindowExW(
        0, L"#32770", L"打开", WS_POPUP, 0, 0, 640, 460,
        nullptr, nullptr, GetModuleHandle(nullptr), nullptr);
    ASSERT_NE(selfFileDlg, nullptr);
    HWND defView = CreateWindowExW(
        0, L"SHELLDLL_DefView", L"", WS_CHILD, 0, 40, 620, 340,
        selfFileDlg, reinterpret_cast<HMENU>(static_cast<INT_PTR>(100)), GetModuleHandle(nullptr), nullptr);
    HWND addrBand = CreateWindowExW(
        0, L"Address Band Root", L"", WS_CHILD, 0, 0, 620, 32,
        selfFileDlg, reinterpret_cast<HMENU>(static_cast<INT_PTR>(41477)), GetModuleHandle(nullptr), nullptr);
    HWND okBtn = CreateWindowExW(
        0, L"BUTTON", L"打开(&O)", WS_CHILD, 460, 400, 80, 28,
        selfFileDlg, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDOK)), GetModuleHandle(nullptr), nullptr);
    ASSERT_NE(defView, nullptr);
    ASSERT_NE(addrBand, nullptr);
    ASSERT_NE(okBtn, nullptr);

    // 默认 allowSelfProcess=false 时过滤当前进程；允许同进程时必须精确识别 Tools3000 自身弹出的 #32770 文件对话框
    EXPECT_FALSE(tools3000::dialog::DialogNavigator::isFileDialog(selfFileDlg, false));
    const auto rejected = tools3000::dialog::DialogNavigator::inspectDialog(selfFileDlg, false);
    EXPECT_EQ(rejected.architecture, tools3000::dialog::DialogArchitecture::Unknown);

    EXPECT_TRUE(tools3000::dialog::DialogNavigator::isFileDialog(selfFileDlg, true));
    const auto accepted = tools3000::dialog::DialogNavigator::inspectDialog(selfFileDlg, true);
    EXPECT_EQ(accepted.architecture, tools3000::dialog::DialogArchitecture::ModernIFileDialog);

    DestroyWindow(selfFileDlg);
}

TEST(DialogQuickJumpAndCompatibilityTest, SanitizesConfigurableHotkeysAndSupportsDisable) {
    using tools3000::dialog::PathMemoryManager;
    // 允许空字符串表示禁用该快捷键，绝不强行写回默认值
    EXPECT_EQ(PathMemoryManager::sanitizeHotkeyString("", "Ctrl+G"), "");
    EXPECT_EQ(PathMemoryManager::sanitizeHotkeyString("   ", "Ctrl+G"), "");
    // 合法组合键规范化输出
    EXPECT_EQ(PathMemoryManager::sanitizeHotkeyString("ctrl+g", "Ctrl+Shift+G"), "Ctrl+G");
    EXPECT_EQ(PathMemoryManager::sanitizeHotkeyString("Ctrl+Shift+G", "Ctrl+G"), "Ctrl+Shift+G");
    EXPECT_EQ(PathMemoryManager::sanitizeHotkeyString("Alt+F2", "Ctrl+G"), "Alt+F2");
    // 不安全裸字母回退至 fallback，杜绝打字劫持
    EXPECT_EQ(PathMemoryManager::sanitizeHotkeyString("G", "Ctrl+G"), "Ctrl+G");
    EXPECT_EQ(PathMemoryManager::sanitizeHotkeyString("not-a-key", "Ctrl+Shift+G"), "Ctrl+Shift+G");
}

TEST(DialogQuickJumpAndCompatibilityTest, ClassifiesFileManagersAndExtractsCleanPaths) {
    using tools3000::dialog::FileManagerKind;
    using tools3000::dialog::classifyFileManagerWindow;
    using tools3000::dialog::extractDirectoryFromFileManagerText;

    EXPECT_EQ(classifyFileManagerWindow(L"CabinetWClass", L"explorer.exe"), FileManagerKind::WindowsExplorer);
    EXPECT_EQ(classifyFileManagerWindow(L"TTOTAL_CMD", L"TOTALCMD64.EXE"), FileManagerKind::TotalCommander);
    EXPECT_EQ(classifyFileManagerWindow(L"dopus.lister", L"dopus.exe"), FileManagerKind::DirectoryOpus);
    EXPECT_EQ(classifyFileManagerWindow(L"ThunderRT6FormDC", L"XYplorer.exe"), FileManagerKind::Xyplorer);
    EXPECT_EQ(classifyFileManagerWindow(L"ATL:00001", L"Q-Dir_x64.exe"), FileManagerKind::QDir);
    EXPECT_EQ(classifyFileManagerWindow(L"WinUIDesktopWin32WindowClass", L"Files.exe"), FileManagerKind::ModernThirdParty);
    EXPECT_EQ(classifyFileManagerWindow(L"HwndWrapper", L"OneCommander.exe"), FileManagerKind::ModernThirdParty);
    EXPECT_EQ(classifyFileManagerWindow(L"Chrome_WidgetWin_1", L"chrome.exe"), FileManagerKind::None);

    // Total Commander TPathPanel 经典尾部 \*.* 与 > 剥离
    EXPECT_EQ(extractDirectoryFromFileManagerText(FileManagerKind::TotalCommander, L"D:\\Projects\\Demo\\*.*"),
              L"D:\\Projects\\Demo");
    EXPECT_EQ(extractDirectoryFromFileManagerText(FileManagerKind::TotalCommander, L"C:\\Users\\Demo\\>"),
              L"C:\\Users\\Demo");
    // XYplorer / Directory Opus 标题与装饰前缀清洗 + UNC 支持 + 零宽方向符剥离
    EXPECT_EQ(extractDirectoryFromFileManagerText(FileManagerKind::Xyplorer, L"\x202A" L"E:/Work/2026 - XYplorer"),
              L"E:\\Work\\2026");
    EXPECT_EQ(extractDirectoryFromFileManagerText(FileManagerKind::DirectoryOpus, L"Address: \\\\nas01\\share\\docs\\"),
              L"\\\\nas01\\share\\docs");
    // 根盘符保留反斜杠
    EXPECT_EQ(extractDirectoryFromFileManagerText(FileManagerKind::WindowsExplorer, L"C:\\"), L"C:\\");
}

TEST(DialogQuickJumpAndCompatibilityTest, ClassifiesNonStandardDialogsSafelyWithoutBlackBoxFalsePositives) {
    using tools3000::dialog::NonStandardDialogKind;
    using tools3000::dialog::classifyNonStandardDialogCandidate;

    // 精准放行 Qt 模态文件框与 JetBrains IDE 对话框
    EXPECT_EQ(classifyNonStandardDialogCandidate(L"Qt6QWindowIcon", L"obs64.exe", true),
              NonStandardDialogKind::QtFileDialog);
    EXPECT_EQ(classifyNonStandardDialogCandidate(L"Qt5QWindowIcon", L"qbittorrent.exe", true),
              NonStandardDialogKind::QtFileDialog);
    EXPECT_EQ(classifyNonStandardDialogCandidate(L"SunAwtDialog", L"idea64.exe", true),
              NonStandardDialogKind::JetBrainsFileDialog);
    EXPECT_EQ(classifyNonStandardDialogCandidate(L"SunAwtDialog", L"pycharm64.exe", false),
              NonStandardDialogKind::JetBrainsFileDialog);

    // 严禁误认标准 #32770（由标准引擎处理）、顶层无 owner 的 Qt 主窗、或纯 GPU 黑盒如 Blender/游戏
    EXPECT_EQ(classifyNonStandardDialogCandidate(L"#32770", L"chrome.exe", true),
              NonStandardDialogKind::None);
    EXPECT_EQ(classifyNonStandardDialogCandidate(L"Qt6QWindowIcon", L"obs64.exe", false),
              NonStandardDialogKind::None);
    EXPECT_EQ(classifyNonStandardDialogCandidate(L"GHOST_WindowClass", L"blender.exe", true),
              NonStandardDialogKind::None);
    EXPECT_EQ(classifyNonStandardDialogCandidate(L"Chrome_WidgetWin_1", L"chrome.exe", true),
              NonStandardDialogKind::None);
}

// -----------------------------------------------------------------------------
// 56. 选区圆角手柄交互与实时调节测试套件
// -----------------------------------------------------------------------------