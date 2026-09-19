#pragma once
// ─────────────────────────────────────────────────────────────────────────────
// WinUtils.h — Windows API 常用操作封装声明
// ─────────────────────────────────────────────────────────────────────────────

#ifndef TOOLS3000_CORE_UTILS_WINUTILS_H
#define TOOLS3000_CORE_UTILS_WINUTILS_H

#include "core/utils/Export.h"
#include "../../common/AtomicFile.h"

#include <windows.h>
#include <string>
#include <string_view>
#include <filesystem>
#include <optional>
#include <vector>

namespace tools3000::core {

class TOOLS3000CORE_API WinUtils {
public:
    /// 获取可执行文件所在目录
    static std::filesystem::path getExeDirectory();

    /// 判断是否处于绿色便携模式 (Portable Mode)
    static bool isPortableMode();

    /// 获取应用数据根目录（测试隔离变量、便携目录、LocalAppData，按此优先级）
    static std::filesystem::path getAppDataDirectory();

    /// 获取日志目录
    static std::filesystem::path getLogDirectory();

    /// 获取配置目录
    static std::filesystem::path getConfigDirectory();

    /// 进程级工作集修剪。仅允许在真正的冷路径调用（插件停用、长截图管线关闭、全部贴图关闭）。
    /// 支持原子时间戳节流（2000ms 内至多修剪一次）以消除高频快照 I/O 抖动。
    /// 级联仅修剪 msedgewebview2 等渲染管线，严格排除全盘搜索服务常驻进程。
    static bool trimWorkingSet(bool force = false);

    /// 判定指定进程是否在工作集修剪白名单内（仅 msedgewebview2.exe 等渲染管线，严格排除 Tools3000_Service/Tools3000_Service）
    static bool shouldTrimProcess(std::wstring_view exeName);

    /// 测试辅助：重置工作集修剪节流时间戳
    static void resetTrimThrottleForTesting();

    /// 由 PID 取得进程可执行文件名 (仅文件名, 如 "chrome.exe")。
    static std::wstring processNameFromPid(DWORD pid);

    /// 获取前台窗口的进程名
    static std::optional<std::wstring> getForegroundProcessName();

    /// 获取窗口类名
    static std::wstring getWindowClassName(HWND hwnd);

    /// 获取窗口标题
    static std::wstring getWindowTitle(HWND hwnd);

    /// wstring → UTF-8 string
    static std::string wstringToUtf8(const std::wstring& wstr);

    /// UTF-8 string 到 wstring
    static std::wstring utf8ToWstring(const std::string& str);

    /// 获取窗口的进程名 (UTF-8)
    static std::string getProcessNameFromWindow(HWND hwnd);

    /// Base64 编码
    static std::string base64Encode(const std::vector<uint8_t>& data);

    /// 字符串转小写
    static std::string toLower(std::string str);

    /// 宽字符串转小写
    static std::wstring toLower(std::wstring str);

    /// 获取文件扩展名对应的 Windows 原生高清图标（自动重建 32 位 ARGB Alpha 通道彻底消灭黑边，并进行单例字典缓存）
    static std::string getFileTypeIconBase64(const std::wstring& extension, bool isDirectory);

    /// 复制宽文本到剪贴板（支持剪贴板占用重试与异常内存释放保护）
    static bool copyToClipboard(const std::wstring& wtext, HWND owner = nullptr);

    /// 复制 UTF-8 文本到剪贴板
    static bool copyToClipboard(const std::string& text, HWND owner = nullptr);

    /// 复制文件路径到剪贴板 (CF_HDROP)，方便用户直接在资源管理器或聊天软件中 Ctrl+V 粘贴文件
    static bool copyFileToClipboard(const std::wstring& filePath, HWND owner = nullptr);

    /// 复制 GIF 动图到剪贴板 (CF_HDROP + CF_DIB + GIF 复合格式)
    /// 微信/QQ等现代聊天软件 Ctrl+V 粘贴时直接识别为可自动循环播放的动图，其它软件粘贴首帧 DIB
    static bool copyGifToClipboard(const std::wstring& filePath, HWND owner = nullptr);

    /// 在 Windows 资源管理器中打开并定位选中指定文件
    static bool showInExplorer(const std::wstring& filePath);

    /// 启用全局高分屏 (DPI) 感知
    /// 解决高分屏下截屏、鼠标手势坐标以及窗口渲染产生的偏移问题
    static void enableHighDpiSupport();

    /// 获取全部多显示器合并后的真实物理边界
    static RECT getVirtualScreenPhysicalBounds();

    /// 获取指定窗口的 DPI 缩放比例 (例如 125% DPI 时返回 1.25)
    static float getDpiScale(HWND hwnd = nullptr);

    /// 零隐私防截策略 (Zero WDA_EXCLUDEFROMCAPTURE)：
    /// 保证 Tools3000 所有 UI 均支持被截图抓取，显式设置 WDA_NONE
    static bool allowWindowCapture(HWND hwnd);

    /// 向后兼容接口：执行零隐私防截策略，强制设置 WDA_NONE 确保能被截图
    static bool excludeWindowFromCapture(HWND hwnd);

    /// 判断窗口是否为桌面背景窗口 (Progman / WorkerW)
    static bool isDesktopWindow(HWND hwnd);

    /// 判断窗口是否为任务栏窗口 (Shell_TrayWnd / Shell_SecondaryTrayWnd)
    static bool isTaskbarWindow(HWND hwnd);

    /// 判断系统界面语言是否为中文
    static bool isSystemLanguageChinese();

    /// 判断指定窗口是否处于全屏独占模式（如 3D 游戏、全屏播放等）
    /// 自动排除桌面、任务栏等系统特殊窗口
    static bool isWindowFullscreen(HWND hwnd);

    /// 查询句柄对应进程是否拥有管理员提升权限
    static bool queryProcessElevated(HANDLE process);

    /// 查询当前主进程是否拥有管理员提升权限
    static bool isCurrentProcessElevated();

    struct WindowProcessQuery {
        bool queryLimitedOk = false;
        bool queryInformationOk = false;
        bool tokenQueryOk = false;
        bool tokenElevated = false;
    };

    /// 探测对本窗口进程的查询权限。勿在 WH_MOUSE_LL 热路径调用。
    static WindowProcessQuery queryWindowProcessAccess(HWND hwnd);

    /// 查询窗口所属进程是否提升权限
    static bool isWindowProcessElevated(HWND hwnd);

    /// 目标完整性更高时，Medium IL 的 WH_MOUSE_LL 收不到事件，PostMessage / SendInput 会被 UIPI 丢掉。
    static bool isWindowHigherIntegrity(HWND hwnd);

    /// 获取当前活动资源管理器（Explorer）或桌面所选中的文件/文件夹完整物理路径
    static std::optional<std::wstring> getSelectedExplorerFile();

    /// 自动捕获当前选中的文本（通过发送 Ctrl+C 并读取剪贴板），若无选中则返回剪贴板现有文本
    static std::string captureSelectedText();

    struct SystemDriveInfo {
        char letter = 0;
        std::wstring path;
        std::wstring volumeLabel;
        std::wstring fileSystem;
        std::wstring typeStr; // "fixed", "remote", "removable", "cdrom", "ramdisk", "unknown"
        uint64_t totalBytes = 0;
        uint64_t freeBytes = 0;
    };

    /// 枚举当前系统所有驱动器（包括本地磁盘、映射网络驱动器、U盘/移动硬盘等）
    static std::vector<SystemDriveInfo> getSystemDrives();

    /// 在 Windows 资源管理器中定位并高亮选中文件或目录。
    /// 采用独立 STA 线程异步解耦与三级降级容灾链 (Tier 1: SHOpenFolderAndSelectItems -> Tier 2: explorer /select -> Tier 3: 打开父目录)
    static bool openFolderAndSelectItem(const std::wstring& filePath);

    /// 非阻塞启动/打开指定文件或应用程序
    static bool openFile(const std::wstring& filePath);

    /// 非阻塞在系统默认浏览器中打开指定 HTTP/HTTPS 网址
    static bool openUrl(const std::wstring& url);

    /// 非阻塞在记事本中打开指定文件
    static bool openWithNotepad(const std::wstring& filePath);

    /// 非阻塞以管理员身份启动程序
    static bool openFileAsAdmin(const std::wstring& filePath);

    /// 非阻塞弹出 Windows 原生文件属性对话框
    static bool showFileProperties(const std::wstring& filePath);

    /// 判断 Windows 系统任务栏是否为深色模式 (用于自适应托盘图标与浮层明暗)
    static bool isSystemTaskbarDark();

    /// 判断 Windows 应用是否为深色模式 (AppsUseLightTheme, 0: Dark, 1: Light)
    static bool isSystemDarkMode();

    /// 获取/初始化进程级 Job Object (带 JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE)
    static HANDLE getProcessJobObject();

    /// 将派生子进程安全加入到当前进程树的 Job Object 中
    static bool assignProcessToCurrentJob(HANDLE hProcess);

    /// 强制将文件句柄对应的未写缓冲刷入物理存储硬件 (FlushFileBuffers)
    static bool flushFileToPhysicalDisk(HANDLE hFile);

    /// 原子级写入文件 (写临时文件 + 物理硬件落盘 + ReplaceFile/MoveFileEx 原子替换)
    static bool atomicWriteFileWithFlush(const std::wstring& targetPath, const std::string& data);

    /// 创建系统低物理内存状态事件通知句柄
    static HANDLE createLowMemoryNotification();

    /// 创建轻量级不可见辅助宿主窗口，用于隔绝 Overlay 窗口在 Windows 任务栏与通知区域产生图标
    static HWND createOverlayHelperOwner(HINSTANCE hInstance, const wchar_t* name = L"Tools3000_OverlayHelperOwner");

    /// 将窗口样式标准化为绝对不污染任务栏与 Alt+Tab 的零泄漏 Overlay 窗口
    static void applyTaskbarSafeOverlayStyle(HWND hwnd, bool excludeFromCapture = true);

    /// 为任意 Win32 / WebView2 宿主窗口赋予跨平台通用圆角 (全兼容 Windows 10/11/Server 2019/2022/2025)
    static void applyUniversalRoundedCorners(HWND hwnd, int width, int height, int radius = 12);

    /// 为指定窗口显式绑定 Application User Model ID (AUMID)
    /// 确保 Windows 10/11 任务栏按键与对应应用组精准归集并展示独立纯净图标
    static bool setWindowAppUserModelId(HWND hwnd, const std::wstring& aumid);

    /// 为指定快捷方式文件 (.lnk) 写入 Application User Model ID (AUMID)
    /// 确保 Windows 10/11 任务栏固定项与开始菜单条目与运行进程精准归集
    static bool setShortcutAppUserModelId(const std::wstring& shortcutPath, const std::wstring& aumid);

    /// 全局同步 Tools3000 桌面与开始菜单快捷方式属性，清除历史遗留旧快捷方式并广播 Windows Shell 刷新通知
    static void syncApplicationShortcuts();

    /// 应急输入状态自愈与清理：强制释放鼠标捕获、解除光标裁剪并使用 VK_F24 中立脉冲安全释放被卡住的修饰键
    static void emergencyFlushInputState() noexcept;
};

}  // namespace tools3000::core

#endif  // TOOLS3000_CORE_UTILS_WINUTILS_H
