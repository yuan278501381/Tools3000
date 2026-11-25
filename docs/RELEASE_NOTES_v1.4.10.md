# Tools3000 v1.4.10 Release Notes | 官方正式版发布说明

**[English](#english-release-highlights)** · **[简体中文说明](#本次重点升级与问题修复)** · **[Upgrade / 升级方式](#升级方式)**

---

## English Release Highlights

Welcome to **Tools3000 v1.4.10** (compared against **v1.4.9**). This feature and performance release brings major upgrades across **Screen Recording Studio**, **Screenshot Text Markup**, **Clipboard Batch Operations**, **Performance Telemetry**, and **Mechanical HDD / Windows 10 LTSC Search Performance** (thanks to Linux.do `@zg2qxy` and `#29`, `@xwshan`):

1. **Screen Recording Studio: All-Format Export Reliability, ~92% Smaller Files & Smart Auto-Zoom**
   - **Full Native 6-Format Export**: Shipped built-in `libx264`, `libvpx (VP9/VP8)`, and `libopus` encoders so **WebM (VP9 + Opus)**, **MP4 (H.264 / HEVC)**, **Animated WebP**, **Discord / Slack / WeChat Compact GIF**, and **HD GIF** all export reliably out of the box with clear localized diagnostics and automatic filename rescue if the target file is locked.
   - **High-Efficiency Desktop Compression**: Adopted CRF/CQ desktop-text-preserving encoding, 5-second GOPs, static-frame VFR decimation, and GIF/WebP duplicate-frame duration merging. On a real 55-second 3456x2086 (51.5 MB) desktop recording, exports shrink by **92–94%** (`WebM VP9 1080p24`: **3.66 MB**, `MP4 H.264 1080p30`: **3.75 MB**, `Compact MP4`: **2.94 MB**) while keeping crisp UI text.
   - **Auto-Zoom Never Crops Upper-Screen Typing**: Overhauled auto-zoom clustering, UIA control filtering, and real-time focus tracking so typing into search bars or inputs in the upper half of the screen stays cleanly framed instead of zooming into the screen center and cutting off your text.
2. **Screenshot Text Box: 8-Way Independent Resizing & Live Reflow**
   - Text markup boxes now feature distinct capsule handles on all four edges alongside corner scale handles: drag **left/right** to widen or narrow the box with live word-wrap reflow (font size unchanged), drag **top/bottom** to adjust box height independently, drag **corners** to scale font size and dimensions proportionally, or **double-click** any edge handle to snap back to auto-fit. Full Undo/Redo fidelity preserved.
3. **Mechanical HDD & Search Engine Overhaul (`-37%` RAM, 5x–100x Faster Queries, Zero First-Open Black Screen)**
   - **-37.4% Search Index RAM & Smooth HDD Snapshots**: Compact 40-byte records and dual-track ASCII / native UTF-16 storage drop real-world index memory on ~3M files from **315 MB to 197 MB** with 100% identical results across all 28 syntax categories; streaming 2 MiB snapshot persistence with smooth HDD pacing (`4 MiB / 12 ms` yield) eliminates memory spikes and disk-queue saturation.
   - **Instant HDD Searches & Zero Stale "No Files Found"**: Moved Recycle Bin/Recent filtering 100% in-memory and bounded disk attribute hydration to the visible top viewport (50 ms HDD budget); upgraded local app/command catalogs to 10-minute background SWR caching so local apps appear immediately without flashing empty states.
   - **Eliminated First-Open Search Black Screen**: Search window now keeps its native loading surface visible until the first rendered frame commits and avoids prematurely suspending an unpainted preloaded WebView.
   - **Smarter Idle Service Memory & Complete Diagnostic Logs**: Added idle high-watermark re-trimming (re-compacting working set if background USN churn from Docker/Syncthing pushes idle usage back above 64 MiB), gentle non-stopping trim on host exit (preserving the session-resident contract), adaptive Settings teardown (**5 min SSD / 15 min HDD**), and automatic inclusion of `Tools3000_Service.log` in exported diagnostic logs.
4. **Performance & Memory Page: Accurate Private Working Set & Instant Quiet Trim (`#29`, `@xwshan`)**
   - Fixed the "Private Working Set" metric reading virtual commit size (`PrivateUsage`) instead of true `PrivateWorkingSetSize`, now matching Windows Task Manager 1:1.
   - Clicking **Run Quiet Trim Now** now trims immediately, refreshes real-time telemetry right away, and gently trims the search service if it is already running (without ever starting a stopped service).
5. **Clipboard (`Alt+V`) Batch Delete Fix**
   - Fixed three interaction issues that caused batch delete to remove only a single item: clicking item checkboxes now toggles selection independently without clearing existing selections; the detail panel's **Delete** button automatically switches to batch delete (`Delete (N)`) whenever multiple entries are checked; and pressing `Delete` after `Ctrl+A` with an empty search box deletes all selected entries directly.

**Upgrade**: In-app via `Settings -> Insights & About -> About -> Check for Updates` (works for both standard installer and portable builds), overwrite with `Tools3000-v1.4.10-Setup.exe` / `Tools3000-v1.4.10-win-x64-portable.zip`, or run `scoop update tools3000`.

---

## 本次重点升级与问题修复

欢迎升级至 **Tools3000 v1.4.10**。本版本以已正式发布的 **v1.4.9** 为对比基线，带来**录屏演播室全格式高压缩与运镜重构**、**截图文本框 8 方向独立拉伸**、**机械硬盘搜索全链路提速与内存直降 37%**、**性能与内存页真实工作集修复**及**剪贴板批量删除根治**五大核心升级（感谢 Linux.do `@zg2qxy`；`#29`, `@xwshan`）：

### 1. 录屏演播室：全 6 格式稳定导出、画质保真体积直降 92%+、智能运镜不再裁切输入框
- **彻底修复 WebM (VP9) 导出失败并完善全格式错误提示**：内置 `libx264`、`libvpx (VP9/VP8)` 与 `libopus` 编码器，实现 **WebM (VP9 + Opus)**、**MP4 (H.264 / HEVC)**、**WebP 动图**、**微信直发 GIF** 与 **高清 GIF** 全 6 格式开箱即用；目标文件被其他程序占用时自动顺延新文件名救援，失败时明确提示具体原因而非笼统报错；
- **桌面专属高压缩算法（清晰小字 + 超小体积）**：引入桌面文字锐利度保护的 CRF/CQ 编码、5 秒长 GOP、静止画面 VFR 智能抽帧以及 GIF/WebP 静止帧时长合并。实测 55 秒 `3456x2086`（`51.5 MB`）原片导出 `1080P` 推荐档后：**WebM (VP9) 仅 `3.66 MB`（省 92.9%）**、**MP4 (H.264) 仅 `3.75 MB`（省 92.7%）**、**紧凑 MP4 仅 `2.94 MB`（省 94.3%）**，文字边缘清晰锐利；
- **重构自动运镜聚焦逻辑，彻底告别上半屏输入被裁切**：修复在屏幕上半部分搜索框/输入框打字时，自动运镜偏向屏幕中心放大导致正在输入的内容移出画面的问题，放大期间始终精准锚定输入焦点与操作意图，确保输入区全程完整可见。

### 2. 截图文本框标注：支持 8 方向独立拉宽、拉长与实时流式换行
- 文本框控制手柄全面升级为**四边独立胶囊手柄 + 四角等比缩放圆点手柄**：
  - 拖动**左右边中点手柄**可独立拉宽或收窄文本框，文字按新宽度实时自动折行重排且**字号保持不变**；
  - 拖动**上下边中点手柄**可独立拉长或压紧文本框高度；
  - 拖动**四个角手柄**继续按比例同步缩放字号与整体框体；
  - **双击任意四边中点手柄**可一键恢复自动贴合文字内容尺寸，完整支持撤销/重做（`Ctrl+Z` / `Ctrl+Y`）。

### 3. 全盘搜索与机械硬盘专项突破：内存直降 37%、消除首次打开黑屏与 90% 磁盘飙升（感谢 Linux.do `@zg2qxy`；`#29`, `@xwshan`）
- **搜索服务内存直降 37.4% 与机械盘平滑快照**：采用紧凑 40 字节索引记录与 ASCII / 原生 UTF-16 双轨存储，近 300 万真实全盘文件专用内存由 **315 MB 降至 197 MB**（28 类全语法结果数量 100% 守恒）；索引快照改为 2 MB 流式分块写出并针对机械盘平滑节流（每 4 MB 让出 12 ms），彻底消除 150–250 MB 临时内存峰值与后台写盘导致的机械盘 90% 磁盘飙升；
- **根治机械盘每次新搜索卡顿数秒与偶发闪现「未找到」**：回收站与历史路径过滤 100% 改为纯内存执行，磁盘属性实时校验严格收敛至首屏视口（机械盘 50 ms 硬预算），大结果集提速 5x–100x；本地应用清单升级为 10 分钟后台无感热更新，搜索框唤起即先呈本地应用与命令，彻底消除“先闪未找到几秒后才出结果”；
- **彻底消除机械盘首次打开搜索框纯色黑屏**：搜索窗在 WebView2 完成真实首帧渲染前保持原生加载提示可见，且不再对尚未完成首帧绘制的后台预热窗口过早挂起冻结，彻底消除首次按 `Alt+Space` 时的数秒纯色黑屏；
- **更稳健的后台服务闲置水位守卫与完整诊断日志**：闲置期间若后台文件变更（如 Docker / Syncthing）再次将服务工作集顶高至 64 MB 以上，将按冷却周期自动温和再修剪；退出主程序前若索引服务在线且非全速常驻模式，也会顺带执行一次轻度瘦身（完整保留搜索服务默认会话常驻策略不变）；设置页销毁周期按介质自适应（固态 5 分钟 / 机械 15 分钟）且不再盲扫休眠盘符；导出诊断日志同步附带 `Tools3000_Service.log` 尾部以便快速排障。

### 4. 修复「性能与内存」页数据偏差与静默回收按钮反馈（`#29`, `@xwshan`）
- **「专用工作集内存」与任务管理器 1:1 对齐**：修复此前误将进程虚拟提交量（`PrivateUsage`）展示为「专用工作集内存」导致数值高于总物理工作集、且回收后数字纹丝不动的缺陷，现统一采集系统真实 `PrivateWorkingSetSize`；
- **「立即执行静默回收」立竿见影**：点击后不再受异步延迟与旧采样缓存影响，立即执行堆紧缩与工作集修剪并实时刷新回落后的内存读数；若全盘搜索服务当前已在线，也会同步执行一次轻度瘦身（未运行时绝不拉起服务）。

### 5. 彻底修复 `Alt+V` 剪贴板批量删除「实际只删了一条」
- **复选框独立勾选不清空选区**：多选后直接点击左侧任意条目的复选框区域即可独立加选或取消，不再因松开 `Ctrl` 键而清空已有选区；
- **右侧详情底栏「删除」按钮多选联动**：勾选多条记录时，详情区底部的「删除」按钮自动切换为红色高亮的「删除 (N)」并触发批量删除，避免误触只删当前焦点单条；
- **支持 `Alt+V` → `Ctrl+A` → `Delete` 一键全删**：当搜索框为空且已勾选多条记录时，直接按 `Delete` 键即可批量删除全部所选条目。

---

## 升级方式

- **应用内一键升级（安装版与绿色便携版均支持）**：打开 `设置 -> 洞察与关于 -> 关于 -> 检查更新`（或点击系统托盘更新提示条）一键下载并自动完成原地升级，完整保留全部配置、剪贴板历史、固定选区与离线模型；
- **标准安装版手动覆盖**：下载下方 `Tools3000-v1.4.10-Setup.exe` 直接运行覆盖安装；
- **绿色便携版手动覆盖**：下载 `Tools3000-v1.4.10-win-x64-portable.zip` 解压覆盖程序目录（保留 `data/` 目录配置与数据）；
- **Scoop 用户**：在终端执行 `scoop update tools3000`。

---

### 文件完整性校验与安全查验 (SHA-256 / VirusTotal)

| 发布文件 | SHA-256 | 安全检测 |
| :--- | :--- | :--- |
| `Tools3000-v1.4.10-Setup.exe` | `0d37d5969a5a6cef29b5a8601d51af07460059b677647713b87a58a3a3d1b7ba` | [VirusTotal 查验](https://www.virustotal.com/gui/file/0d37d5969a5a6cef29b5a8601d51af07460059b677647713b87a58a3a3d1b7ba) |
| `Tools3000-v1.4.10-win-x64-portable.zip` | `bd5f8a8460ed6c7fab6cfeae8add967ebb5326da2725f0be56744160ce5ab49c` | [VirusTotal 查验](https://www.virustotal.com/gui/file/bd5f8a8460ed6c7fab6cfeae8add967ebb5326da2725f0be56744160ce5ab49c) |

下载后可在 PowerShell 中快速核对本地文件哈希：

```powershell
Get-FileHash .\Tools3000-v1.4.10-Setup.exe -Algorithm SHA256
```

> 如遇 Windows Defender SmartScreen 未知发布者提示，核对上述 SHA-256 与 VirusTotal 无误后点击「更多信息 -> 仍要运行」即可；安装包与便携包内均随附 SBOM.spdx.json 软件物料清单。
