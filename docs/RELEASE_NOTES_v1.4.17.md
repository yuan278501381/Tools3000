# Tools3000 v1.4.17 Release Notes | 官方正式版发布说明

**[English](#english-release-highlights)** · **[简体中文说明](#本次重点更新与体验改进)** · **[Upgrade / 升级方式](#升级方式)**

---

## English Release Highlights

Welcome to **Tools3000 v1.4.17** (compared against **v1.4.16**). This targeted reliability release ensures the on-demand file search service starts completely clean (0 processes, 0 MB) after every Windows shutdown and boot — even when **Windows Fast Startup (Hybrid Shutdown)** is enabled:

1. **Clean Search Service Exit on Windows Fast Startup & User Logoff**
   - Fixed a Windows Fast Startup edge case where clicking **Shut down** logged off the user session while Windows hibernated Session 0 along with a previously launched File Index Service, causing the old service process to reappear on the next boot before you ever opened Search.
   - The search service now listens for user session logoff and system shutdown notifications: once all interactive user sessions have logged off (including during Windows Fast Startup shutdown) or the system shuts down, it flushes the latest incremental file index snapshot to disk and stops cleanly.
   - **Zero impact during normal use**: locking the screen, putting the PC to sleep, disconnecting Remote Desktop without logging off, restarting the Tools3000 main app, or switching accounts while another user stays signed in all keep instant Search warm and responsive.
   - Because the latest incremental snapshot is flushed cleanly before stopping, the next time you press `Alt+Space` after booting, the search index resumes in seconds without needing a full drive rescan.

**Upgrade**: In-app via `Settings -> Insights & About -> About -> Check for Updates` (supports both installer and portable builds), overwrite with `Tools3000-v1.4.17-Setup.exe` / `Tools3000-v1.4.17-win-x64-portable.zip`, or run `scoop update tools3000`.

---

## 本次重点更新与体验改进

欢迎升级至 **Tools3000 v1.4.17**。本版本以已正式发布的 **v1.4.16** 为对比基线，带来针对 Windows 快速启动场景的专项内存与生命周期加固修复：

### 1. 修复 Windows 快速启动「关机再开机」后搜索索引服务未按需清零的问题
- **彻底解决关机重启后“未搜文件却见服务常驻”**：Windows 10/11 默认开启「快速启动（混合关机）」，点击「关机」时会注销用户会话，但会把 Session 0 中前一次搜索留下的索引服务进程连同内存一并冻结进休眠镜像，导致下次按电源键开机时旧服务被原样复活；
- **全用户注销与系统关机前自动落盘停服**：索引服务现已接入用户会话注销与系统关机事件。当本机所有登录用户均注销（含快速启动关机前自动注销）或系统关机时，服务会先将最新增量文件索引快照安全落盘，再优雅退出，确保**每次关机后再开机都是彻底的 0 索引服务进程、0 MB 占用**，直到您显式按下 `Alt+Space` 搜文件时才会按需启动，且能秒级加载关机前保存的最新快照免重扫全盘；
- **日常使用零干扰**：普通锁屏、电脑睡眠/合盖恢复、远程桌面断开不注销、退出重开 Tools3000 主程序，以及多用户电脑仍有其他账号在线时，均完全保持当前会话内秒开常驻不中断。

---

## 升级方式

- **应用内一键升级（安装版与绿色便携版均支持）**：打开 `设置 -> 洞察与关于 -> 关于 -> 检查更新`（或点击系统托盘更新提示条）一键下载并自动完成原地升级，完整保留全部配置、剪贴板历史、固定选区与离线模型；
- **标准安装版手动覆盖**：下载下方 `Tools3000-v1.4.17-Setup.exe` 直接运行覆盖安装；
- **绿色便携版手动覆盖**：下载 `Tools3000-v1.4.17-win-x64-portable.zip` 解压覆盖程序目录（保留 `data/` 目录配置与数据）；
- **Scoop 用户**：在终端执行 `scoop update tools3000`。

---

### 文件完整性校验与安全查验 (SHA-256 / VirusTotal)

| 发布文件 | SHA-256 | 安全检测 |
| :--- | :--- | :--- |
| `Tools3000-v1.4.17-Setup.exe` | `08dedb02fe5b1fdf6e92189facb5b9408da296b30cf7ff6a4127a594f27423d8` | [VirusTotal 查验](https://www.virustotal.com/gui/file/08dedb02fe5b1fdf6e92189facb5b9408da296b30cf7ff6a4127a594f27423d8) |
| `Tools3000-v1.4.17-win-x64-portable.zip` | `8a383cfa8f28e6f9c1059188550b9181ff9d96810e7c86de6197425d248026a1` | [VirusTotal 查验](https://www.virustotal.com/gui/file/8a383cfa8f28e6f9c1059188550b9181ff9d96810e7c86de6197425d248026a1) |

下载后可在 PowerShell 中快速核对本地文件哈希：

```powershell
Get-FileHash .\Tools3000-v1.4.17-Setup.exe -Algorithm SHA256
```

> 如遇 Windows Defender SmartScreen 未知发布者提示，核对上述 SHA-256 与 VirusTotal 无误后点击「更多信息 -> 仍要运行」即可；安装包与便携包内均随附 SBOM.spdx.json 软件物料清单。
