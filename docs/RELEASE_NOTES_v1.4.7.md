# Tools3000 v1.4.7 Release Notes | 官方正式版发布说明

**[English](#english-release-highlights)** · **[简体中文说明](#本次重点修复与体验优化)** · **[Upgrade / 升级方式](#升级方式)** · **[SHA-256 & VirusTotal](#文件完整性校验与安全查验-sha-256--virustotal)**

---

## English Release Highlights

Welcome to **Tools3000 v1.4.7** (compared against **v1.4.6**). This focused stability and memory-hygiene release fixes a startup crash on **Windows Server 2019**, eliminates background idle working-set oscillation, resolves duplicate image insertion when dragging from Clipboard History into AI IDEs or browsers, and clears all open security advisories:

1. **Windows Server 2019 Startup Crash Fix**
   - Resolved a crash occurring roughly 5 seconds after launch on Windows Server 2019 (Build 17763) systems when post-startup memory cleanup overlapped with slower WebView2 tray initialization. Startup cleanup now defers automatically while WebView2 is initializing and strictly compacts only the application's own heap.
2. **Rock-Solid Idle Memory Stability (No More 3 MB <-> 40+ MB Swings)**
   - **On-Demand Gesture Surface Allocation & 30s Idle Reclaim**: Full-screen 4K gesture trail surfaces are no longer allocated at cold startup; they are created on demand on your first gesture and automatically reclaimed with GPU memory trim after 30 seconds of inactivity, keeping gesture response instantaneous while saving ~30 MB of idle footprint.
   - **Quiet Background Standby**: Reduced unnecessary background wakeups when the Performance & Memory page is closed (2s -> 30s adaptive sampling) and relaxed foreground fullscreen checks, eliminating millions of idle page faults. Added a gentle idle memory watermark guard that keeps steady-state standby calm without causing thrashing.
3. **Clipboard History (`Alt + V`): Single-Copy Image & File Drag-to-Paste**
   - Fixed an issue where dragging an image or file from Clipboard History into **Antigravity**, **Cursor**, or modern browsers (**Chrome / Edge / Firefox**) inserted two identical copies instead of one. Native drag-and-drop consumption is now tracked accurately for image/file payloads while keeping single-paste plain-text drag and native `@Link` URL cards intact.
4. **Security Dependency Update & 8-Language Documentation Polish**
   - Upgraded UI internationalization library `i18next` to `26.7.2` (resolving Dependabot alert `#42`, leaving **0 open security alerts**).
   - Completed cross-regional localization polish across all 8 language READMEs and the 13-module visual overview posters.

**Upgrade**: In-app via `Settings -> Insights & About -> About -> Check for Updates` (works for both standard installer and portable builds), overwrite with `Tools3000-v1.4.7-Setup.exe` / `Tools3000-v1.4.7-win-x64-portable.zip`, or run `scoop update tools3000`.

---

## 本次重点修复与体验优化

欢迎升级至 **Tools3000 v1.4.7**。本版本以已正式发布的 **v1.4.6** 为对比基线，聚焦系统兼容性稳定性、长时静置内存曲线治理与剪贴板拖拽体验打磨：

### 1. 彻底修复 Windows Server 2019 启动约 5 秒托盘闪退
- 修复在 Windows Server 2019（Build 17763）安装 WebView2 后启动 Tools3000，出现「启动成功」提示约 5 秒后托盘图标意外闪退的问题；
- 启动后的静默内存整理改为在 WebView2 初始化完成前自动顺延，并收敛堆整理范围仅作用于自身进程堆，确保服务器系统与慢速虚拟机同样稳定常驻。

### 2. 长时静置内存稳态治理（告别 `3 MB <-> 40+ MB` 锯齿波动）
- **手势全屏画布零预占 + 30 秒闲置自动回收**：冷启动不再提前占用高分屏/4K 全屏手势显存表面，改为首次划动手势时按需瞬时分配，闲置满 30 秒自动释放并回收显存缓存；跟手速度零损耗，同时消除冷态与用后常驻的数十 MB 显存占用；
- **后台静默降噪与温和水位巡检**：未打开「性能与内存」页面时自动放缓后台监控采样频率（由 2 秒降至 30 秒），同步降低前台全屏状态检查频率，大幅削减无操作期间的后台软缺页增长；主循环增加温和的闲置内存水位巡检，长时后台挂机内存曲线更加平稳（实机冷态稳态工作集约 `2.7–5.5 MB`）。

### 3. 修复剪贴板（`Alt + V`）拖入图片或文件至 Antigravity / 浏览器出现双图重复
- 修复从剪贴板面板拖拽图片或文件到 Antigravity 对话框、Cursor 或现代浏览器（Chrome / Edge / Firefox）时，因系统原生拖放成功后又重复触发一次回退粘贴、导致同一张图片被插入两份的问题；
- 纯文本绕过网页文件上传遮罩层、单条 `http(s)://` 网址生成 `@Link` 卡片以及跨权限窗口兜底直贴体验均完整保留。

### 4. 安全依赖升级与 8 国语言文档精修
- 升级前端多语言核心依赖 `i18next` 至 `26.7.2`，消除 Dependabot `#42` 告警（当前仓库开放安全告警归零）；
- 同步完成全 8 语种说明文档与 13 核功能总览海报的跨地域本地化精修。

---

## 升级方式

- **应用内一键升级（安装版与绿色便携版均支持）**：打开 `设置 -> 洞察与关于 -> 关于 -> 检查更新`（或点击系统托盘更新提示条）一键下载并自动完成原地升级，完整保留全部配置、剪贴板历史、固定选区与离线模型；
- **标准安装版手动覆盖**：下载下方 `Tools3000-v1.4.7-Setup.exe` 直接运行覆盖安装；
- **绿色便携版手动覆盖**：下载 `Tools3000-v1.4.7-win-x64-portable.zip` 解压覆盖程序目录（保留 `data/` 目录配置与数据）；
- **Scoop 用户**：在终端执行 `scoop update tools3000`。

---

### 文件完整性校验与安全查验 (SHA-256 / VirusTotal)

| 发布文件 | SHA-256 | 安全检测 |
| :--- | :--- | :--- |
| `Tools3000-v1.4.7-Setup.exe` | `533b610bc4b2e5db06cc86d1775bbe7092d8a3d1c7ae192dbbbef6cef9829136` | [VirusTotal 查验](https://www.virustotal.com/gui/file/533b610bc4b2e5db06cc86d1775bbe7092d8a3d1c7ae192dbbbef6cef9829136) |
| `Tools3000-v1.4.7-win-x64-portable.zip` | `c1cf8720984b9f85c0cce67dc3d30305ae7044b521505d2d9cea949281d1e138` | [VirusTotal 查验](https://www.virustotal.com/gui/file/c1cf8720984b9f85c0cce67dc3d30305ae7044b521505d2d9cea949281d1e138) |

下载后可在 PowerShell 中快速核对本地文件哈希：

```powershell
Get-FileHash .\Tools3000-v1.4.7-Setup.exe -Algorithm SHA256
```

> 如遇 Windows Defender SmartScreen 未知发布者提示，核对上述 SHA-256 与 VirusTotal 无误后点击「更多信息 -> 仍要运行」即可；安装包与便携包内均随附 SBOM.spdx.json 软件物料清单。
