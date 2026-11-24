#pragma once
// ─────────────────────────────────────────────────────────────────────────────
// OutOfProcessFileDialog.h — 短命子进程隔离的系统文件与目录选择对话框 (SSOT)
//
// 架构目的:
//   Windows Shell 的 IFileOpenDialog / IFileSaveDialog / GetSaveFileNameW 在打开时
//   会将本机安装的第三方资源管理器外壳扩展（如网盘同步图标扩展及整套 .NET 4.0 CLR:
//   clr.dll / mscorlib.ni.dll 60+ MB + GC 线程）永久加载进调用进程且无法卸载。
//   通过复用 Tools3000.exe --worker=file-dialog 短命子进程执行对话框交互，选完即退，
//   使常驻主进程保持 0 CLR 污染、0 外壳扩展野线程与极低内存水位；子进程异常时自动安全回退进程内执行。
// ─────────────────────────────────────────────────────────────────────────────

#ifndef TOOLS3000_CORE_UTILS_OUTOFPROCESSFILEDIALOG_H
#define TOOLS3000_CORE_UTILS_OUTOFPROCESSFILEDIALOG_H

#include "core/utils/Export.h"

#include <windows.h>

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace tools3000::core {

enum class FileDialogMode : uint8_t {
    OpenFile = 0,
    SaveFile = 1,
    PickFolder = 2,
};

struct FileDialogFilterSpec {
    std::wstring label;
    std::wstring pattern;
};

struct FileDialogRequest {
    FileDialogMode mode = FileDialogMode::OpenFile;
    HWND ownerHwnd = nullptr;
    std::wstring title;
    std::wstring defaultExtension;
    std::wstring defaultFileName;
    std::filesystem::path initialDirectory;
    std::vector<FileDialogFilterSpec> filters;
    uint32_t initialFilterIndex = 1; // 1-based, 与 COMDLG_FILTERSPEC / OPENFILENAMEW 对齐
    bool dontAddToRecent = true;

    // 仅用于自动化端到端单测：非空时在子进程内验证完 STA COM 环境后直接返回该路径，不阻塞人工点击
    std::filesystem::path autoSelectPathForTest;
};

struct FileDialogResult {
    bool accepted = false;
    bool cancelled = false;
    bool ranOutOfProcess = false;
    uint32_t selectedFilterIndex = 1; // 1-based
    std::filesystem::path selectedPath;
};

class TOOLS3000CORE_API OutOfProcessFileDialog final {
public:
    OutOfProcessFileDialog() = delete;

    /// 在短命隔离 Worker 子进程中弹出系统文件/目录对话框（若子进程拉起失败则无感回退进程内执行）
    static FileDialogResult show(const FileDialogRequest& request);

    /// 进程内直接执行（供 Worker 子进程主入口与极端兜底复用，要求调用线程已初始化 STA COM）
    static FileDialogResult executeInProcess(const FileDialogRequest& request);

    /// --worker=file-dialog --job=<path> 子进程入口（返回进程退出码：0=正常完成或取消，非0=参数异常）
    static int runWorkerFromJobFile(const std::filesystem::path& jobFilePath);
};

} // namespace tools3000::core

#endif // TOOLS3000_CORE_UTILS_OUTOFPROCESSFILEDIALOG_H
