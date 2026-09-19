/**
 * Tools3000 - High Performance Windows Productivity Suite
 * 
 * Copyright (c) 2026 Yy1 (GitHub yuan278501381) <https://github.com/yuan278501381> & Tools3000 contributors
 * 
 * Licensed under the MIT License.
 */

#pragma once

#include <string>
#include <windows.h>

namespace tools3000::dialog {

// 文件对话框架构类型
enum class DialogType {
    Unknown,       // 未知
    Modern,        // IFileOpenDialog / IFileSaveDialog (Vista+ COM, VS Code / Antigravity IDE / 大多数现代应用)
    Legacy,        // GetOpenFileName / OPENFILENAME (#32770, 老式应用)
    FolderPicker,  // 文件夹选择器 (SHBrowseForFolder, SysTreeView32 / NamespaceTreeControl)
};

class DialogNavigator {
public:
    static DialogNavigator& instance();

    // 尝试将目标文件对话框导航至指定文件夹路径（全球化通用、零语言依赖、杜绝误选）
    bool navigateToFolder(HWND dialogHwnd, const std::string& folderPath);

    // 检查某个窗口句柄是否为有效的文件打开/保存/文件夹选择对话框
    static bool isFileDialog(HWND hwnd, bool allowCurrentProcess = false);

    // 检查某个窗口句柄是否为文件传输/复制/移动/删除等进度操作弹窗
    static bool isProgressOrTransferDialog(HWND hwnd);

    // 检查指定进程名是否属于软件安装程序/卸载程序
    static bool isInstallerProcess(const std::string& processName);

    // 检查窗口或其宿主是否属于软件安装向导/卸载向导 (结合进程名、向导标题与窗口层级特征多维判定)
    static bool isInstallerOrWizard(HWND dialogHwnd);

    // 检测对话框类型 (Modern IFileDialog / Legacy OPENFILENAME / FolderPicker)
    static DialogType detectDialogType(HWND hwnd);

    // 查找对话框内的文件名/路径输入控件句柄
    static HWND findPathEditControl(HWND dialogHwnd);

    // 查找对话框顶层地址栏控件句柄
    static HWND findAddressBandControl(HWND dialogHwnd);

    // 查找对话框中间文件列表视图句柄 (SHELLDLL_DefView / DirectUIHWND)
    static HWND findShellViewControl(HWND dialogHwnd);

    // 查找对话框确认/打开按钮句柄 (IDOK)
    static HWND findOkButtonControl(HWND dialogHwnd);

    // 实时提取对话框当前所在的真实文件夹绝对物理路径
    static std::string getCurrentDialogFolder(HWND dialogHwnd);

    // 实时提取用户当前选定/输入的目标项绝对物理路径
    static std::string getSelectedPath(HWND dialogHwnd);

private:
    DialogNavigator() = default;
    ~DialogNavigator() = default;

    // UIAutomation: 读取地址栏当前路径 (Modern IFileDialog)
    static std::string uiaGetAddressBarPath(HWND dialogHwnd);

    // UIAutomation: 读取文件名输入框文本 (Modern IFileDialog)
    static std::string uiaGetFileNameText(HWND dialogHwnd, bool* controlFound = nullptr);

    // UIAutomation: 将路径写入地址栏并导航 (Modern IFileDialog)
    static bool uiaNavigate(HWND dialogHwnd, const std::wstring& wPath);
};

} // namespace tools3000::dialog
