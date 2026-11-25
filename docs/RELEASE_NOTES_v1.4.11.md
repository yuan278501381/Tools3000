# Tools3000 v1.4.11 Release Notes | 官方正式版发布说明

**[English](#english-release-highlights)** · **[简体中文说明](#本次快速修复说明)** · **[Upgrade / 升级方式](#升级方式)**

---

## English Release Highlights

Welcome to **Tools3000 v1.4.11** (compared against **v1.4.10**). This hotfix release eliminates the "Setup was unable to automatically close all applications" prompt during in-app updates and hardens clean process exit:

1. **Zero-Prompt In-App Auto-Updates (Including Upgrades from Older Versions)**
   - Fixed an issue where clicking **Install Update** in Settings could trigger Windows Restart Manager's *"Setup was unable to automatically close all applications"* dialog before the installer's built-in process cleanup ran.
   - Installer silent/auto-update mode now cleanly waits for and clears any lingering old `Tools3000.exe` processes **before** entering the preparing phase, so even users upgrading from `v1.4.10` or earlier experience a completely smooth, prompt-free in-place update.
   - Switched in-app updater and CLI install parameters to `/NOCLOSEAPPLICATIONS /NORESTARTAPPLICATIONS` so process handover and relaunch are managed exclusively by the installer without Restart Manager interference.
2. **Zombie-Proof Process Exit Guard**
   - Moved the exit watchdog ahead of `CoUninitialize()` (after all settings, backups, hotkeys, and logs are safely flushed and the single-instance mutex is released) and added an 8-second outer shutdown fuse, preventing WebView2/COM teardown stalls from ever leaving a windowless process locking files in the install directory.

**Upgrade**: In-app via `Settings -> Insights & About -> About -> Check for Updates` (works for both standard installer and portable builds), overwrite with `Tools3000-v1.4.11-Setup.exe` / `Tools3000-v1.4.11-win-x64-portable.zip`, or run `scoop update tools3000`.

---

## 本次快速修复说明

欢迎升级至 **Tools3000 v1.4.11**。本版本以已正式发布的 **v1.4.10** 为对比基线，专项根治应用内一键覆盖更新时偶发弹出「安装程序无法自动关闭所有应用程序」报错框的问题：

### 1. 彻底消除应用内一键更新「无法自动关闭所有应用程序」报错弹窗
- **向下兼容所有存量旧版本一键升级**：安装包在进入准备阶段（Windows Restart Manager 扫描）之前，即抢先等待旧进程优雅保存退出并自动清理残留文件锁，确保从 `v1.4.10` 及更早版本点击软件内一键升级也能全程零报错弹窗、自动平滑覆盖并拉起新版；
- **关闭冗余 Restart Manager 覆盖参数**：应用内更新与 CLI 静默安装统一改由安装器原生清场与拉起逻辑接管，不再因命令行覆盖配置而误弹 Restart Manager 警告框。

### 2. 加固主程序退出阶段防僵死守护
- 将退出防僵死看门狗前置至 `CoUninitialize()` 之前（在全部配置落盘、自动备份、热键注销、单实例互斥体释放及日志刷盘完成后启动），并补齐顶层退出熔断保护，彻底杜绝 WebView2/COM 卸载阻塞导致后台残留无窗口进程锁住安装目录。

---

## 升级方式

- **应用内一键升级（安装版与绿色便携版均支持）**：打开 `设置 -> 洞察与关于 -> 关于 -> 检查更新`（或点击系统托盘更新提示条）一键下载并自动完成原地升级，完整保留全部配置、剪贴板历史、固定选区与离线模型；
- **标准安装版手动覆盖**：下载下方 `Tools3000-v1.4.11-Setup.exe` 直接运行覆盖安装；
- **绿色便携版手动覆盖**：下载 `Tools3000-v1.4.11-win-x64-portable.zip` 解压覆盖程序目录（保留 `data/` 目录配置与数据）；
- **Scoop 用户**：在终端执行 `scoop update tools3000`。

---

### 文件完整性校验与安全查验 (SHA-256 / VirusTotal)

| 发布文件 | SHA-256 | 安全检测 |
| :--- | :--- | :--- |
| `Tools3000-v1.4.11-Setup.exe` | `7b0633244ba2d9f9dadfa0ac1a03bffe678b527de202f6a1e6c04b60fc26487d` | [VirusTotal 查验](https://www.virustotal.com/gui/file/7b0633244ba2d9f9dadfa0ac1a03bffe678b527de202f6a1e6c04b60fc26487d) |
| `Tools3000-v1.4.11-win-x64-portable.zip` | `e51521250995593e1936f9036822c81a5fa4291f026a5e9bdc52ac150c3732f5` | [VirusTotal 查验](https://www.virustotal.com/gui/file/e51521250995593e1936f9036822c81a5fa4291f026a5e9bdc52ac150c3732f5) |

下载后可在 PowerShell 中快速核对本地文件哈希：

```powershell
Get-FileHash .\Tools3000-v1.4.11-Setup.exe -Algorithm SHA256
```

> 如遇 Windows Defender SmartScreen 未知发布者提示，核对上述 SHA-256 与 VirusTotal 无误后点击「更多信息 -> 仍要运行」即可；安装包与便携包内均随附 SBOM.spdx.json 软件物料清单。
