#pragma once
// ─────────────────────────────────────────────────────────────────────────────
// FileLocksmithEngine.h — 文件/文件夹占用检测与安全解锁双引擎 (Tools3000Core)
//
// 架构特性:
//   1. 双引擎混合扫描 (0 常驻内存，按需触发):
//      - 主引擎: Windows Restart Manager API (rstrtmgr.dll 动态加载，5~30ms 极速检出)
//      - 深度引擎: NtQuerySystemInformation(64) + 独立超时线程 CancelSynchronousIo 护栏
//                  (支持目录/CWD/隐藏句柄/DLL 镜像映射，100% 防 Named Pipe 内核永久挂死)
//   2. 安全分级护栏: 严禁强杀系统关键进程 (PID 0/4, csrss/lsass/wininit/smss 等)，
//      对 explorer.exe 提供专属安全重启操作。
//   3. 纯 HKCU 零注入右键菜单注册器: 支持普通右键与 Extended (Shift+右键) 双模式，
//      0 管理员权限、0 DLL 注入资源管理器、支持便携与安装双形态。
// ─────────────────────────────────────────────────────────────────────────────

#ifndef TOOLS3000_CORE_UTILS_FILELOCKSMITHENGINE_H
#define TOOLS3000_CORE_UTILS_FILELOCKSMITHENGINE_H

#include "core/utils/Export.h"

#include <windows.h>
#include <cstdint>
#include <string>
#include <vector>

namespace tools3000::core {

enum class LockProcessRisk {
    Normal = 0,
    Explorer = 1,
    CriticalSystem = 2
};

struct FileLockProcessInfo {
    DWORD pid = 0;
    std::wstring exeName;
    std::wstring exePath;
    std::wstring displayName;
    std::vector<std::wstring> lockedPaths;
    std::vector<uintptr_t> handleValues;
    LockProcessRisk risk = LockProcessRisk::Normal;
    bool viaRestartManager = false;
    bool viaHandleScan = false;
};

struct FileLockScanResult {
    bool ok = false;
    bool isDirectory = false;
    bool currentProcessElevated = false;
    uint32_t inaccessibleProcessCount = 0;
    uint64_t elapsedMs = 0;
    std::wstring targetPath;
    std::vector<FileLockProcessInfo> processes;
};

class TOOLS3000CORE_API FileLocksmithEngine {
public:
    /// 对指定文件或文件夹执行双引擎占用扫描（支持单句柄超时保护与总预算上限）
    static FileLockScanResult scanTargetLocks(const std::wstring& targetPath,
                                              uint32_t totalBudgetMs = 3500);

    /// 判定进程安全风险等级（硬保护系统关键进程防蓝屏，单独标记 explorer.exe）
    static LockProcessRisk classifyProcessRisk(DWORD pid, const std::wstring& exeName) noexcept;

    /// 安全终止占用进程（拒绝 CriticalSystem；优先温和 WM_CLOSE 再 TerminateProcess）
    static bool terminateProcessSafely(DWORD pid, std::string& outError);

    /// 安全重启 Windows 资源管理器 (explorer.exe) 以释放外壳预览/目录锁
    static bool restartExplorerSafely(std::string& outError);

    /// 同步 HKCU 资源管理器右键菜单（支持文件 * 与文件夹 Directory，含 Shift+右键 Extended 选项）
    static bool syncExplorerContextMenu(bool enabled,
                                        bool shiftOnlyExtended,
                                        const std::wstring& exePath,
                                        const std::wstring& menuLabel);

    /// 查询当前 HKCU 右键菜单注册状态
    static bool isExplorerContextMenuRegistered(bool* outShiftOnly = nullptr);

    /// 独立进程零常驻拉起 File Locksmith 弹窗 (Tools3000.exe --unlock "<path>")
    static bool launchUnlockWindowProcess(const std::wstring& targetPath, bool requestElevation = false);
};

}  // namespace tools3000::core

#endif  // TOOLS3000_CORE_UTILS_FILELOCKSMITHENGINE_H
