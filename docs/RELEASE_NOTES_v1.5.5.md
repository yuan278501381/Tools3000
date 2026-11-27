# Tools3000 v1.5.5 Release Notes | 官方正式版发布说明

**[English](#english-release-highlights)** · **[简体中文说明](#本次重点更新与体验改进)** · **[Upgrade / 升级方式](#升级方式)**

---

## English Release Highlights

Welcome to **Tools3000 v1.5.5** (compared against **v1.5.4**). This release delivers a comprehensive **Window Manager v2** upgrade — still **completely disabled by default** (zero background processes, zero hooks, zero RAM until you opt in) — bringing a dual-layer visual layout manager, red-frame workspace capture with interactive desktop & taskbar previews, a freely resizable `Alt+Tab` switcher plus standalone same-app `Alt+\`` switching, and seamless multi-monitor / virtual-desktop resilience across Windows 10, Windows 11, and Windows Server 2019–2025:

1. **SnapZones v2 (FancyZones): Dual-Layer Visual Layout Hub & 6 Parametric Templates**
   - Launch the new **Dual-Layer Layout Manager** directly from `Settings -> Window Manager` or via hotkey (`Win+Shift+~`): see semi-transparent snap zones live across your full desktop while adjusting templates, zone counts, and spacing right on top.
   - Ships with **6 built-in parametric templates** (No Layout, Focus, Columns, Rows, Grid, and Priority Grid), custom layout duplication & 1:1 fullscreen visual editing, **0–9 instant layout hotkeys** with conflict protection, and automatic landscape/portrait defaults per monitor.
   - Offline monitors are safely preserved instead of lost when docking/undocking, and bindings for deleted virtual desktops can be audited and cleaned up in one click.
2. **Workspaces v2: Red-Frame Capture, Interactive Preview Stage & One-Click Desktop Shortcuts**
   - Clicking **Create Workspace** now frames every active monitor with a crisp `4px` crimson capture border so you can arrange your real desktop live and capture your current virtual desktop snapshot (with real app icons, launch arguments, administrator elevation flags, minimized window positions, and matched SnapZones).
   - Inspect and fine-tune workspaces in the new **interactive miniature desktop & taskbar preview stage** — hovering any app row or preview tile highlights it live, with accordion controls for launch arguments, elevation, window state, exact bounds, SnapZone slot, always-on-top, and opacity.
   - Generate standalone **Desktop Shortcuts (`.lnk`)** to launch any workspace in one double-click, with automatic multi-monitor collapse fallback when switching from multi-screen desks to a single laptop screen and a lightweight floating launch progress card.
3. **Quick Switch v2: Freely Resizable `Alt+Tab` Hub & Standalone Same-App `Alt+\``**
   - The enhanced `Alt+Tab` switcher now opens in a roomy dual-pane layout that supports **8-direction edge/corner resizing, titlebar dragging, adjustable list/preview split ratio, and cross-monitor position memory** (with automatic rescue if a remembered monitor is disconnected).
   - Added **in-place desktop position preview**, Chinese Pinyin initial & fuzzy filtering, full keyboard/button window controls (`F4`–`F9`, `P` pin, `Z` maximize/restore, `T` minimize to tray, plus tray-window recovery), and a **standalone `Alt+\`` same-app window switcher** that works independently even if you prefer to keep Windows' built-in `Alt+Tab`.
4. **Window Management Mouse Gestures & Broad Windows Compatibility**
   - Added **6 built-in mouse gesture commands** for Window Manager (Dual-Layer Layout Hub, Capture Workspace, Launch Workspace, Quick Switch, Toggle Always-on-Top, and Cycle Opacity).
   - Engineered and verified across **Windows 10 (1809+), Windows 11, Windows Server 2019 / 2022 / 2025, RDP, x64, and ARM64**, with cold-path memory trimming returning idle working set to under `1 MB`.

**Upgrade**: In-app via `Settings -> Insights & About -> About -> Check for Updates` (supports both installer and portable builds), overwrite with `Tools3000-v1.5.5-Setup.exe` / `Tools3000-v1.5.5-win-x64-portable.zip`, or run `scoop update tools3000`.

---

## 本次重点更新与体验改进

欢迎升级至 **Tools3000 v1.5.5**。本版本以已正式发布的 **v1.5.4** 为对比基线，带来全新进化的 **窗口管理套件 v2.0**（延续出厂默认全关闭设计，未开启时零进程、零钩子、零内存占用），全面重构分屏布局、工作区一键快照与增强 `Alt+Tab` 切换体验，并针对多显示器热插拔、虚拟桌面及 Windows 10 / Windows 11 / Windows Server 2019–2025 全场景做了系统级加固：

### 1. 分屏布局 v2（FancyZones）：双层共视布局管理器与 6 大参数化模板
- 全新上线 **双层共视布局管理器**（支持从 `设置 -> 窗口管理` 或全局热键一键唤起）：底层直接全屏实时透视当前分屏分区，顶层悬浮卡片控制台实时切换布局、增减分区数量或开关分区间距，所见即所得；
- 内置 **6 套参数化模板**（无布局、焦点、列、行、网格、优先级网格），支持自定义布局复制与 1:1 全屏精细编辑、**0–9 数字布局快捷键**一键切换与冲突自动消解，以及横屏/竖屏默认布局自适应；
- 新增**多屏与虚拟桌面拓扑自愈**：外接显示器拔出时自动保留历史绑定配置（插回即恢复），已删除的虚拟桌面绑定支持在设置页一键扫描并安全清理。

### 2. 工作区 v2：正红边框实时摆拍、双层微缩预览舞台与桌面快捷方式
- 点击 **创建工作区** 后，系统将在所有活动显示器外缘拉起清晰的 `4px` 正红取景细边，方便你先将桌面窗口摆放到理想位置再一键捕获，精准记录当前虚拟桌面内所有窗口的真实彩色图标、启动参数、管理员权限标识、最小化窗还原位置及所属分屏槽位；
- 捕获与编辑界面全面升级为 **双层交互式桌面与任务栏微缩舞台**：鼠标悬停在应用列表或微缩预览窗口上可实时双向高亮联动，展开手风琴面板即可逐项精调管理员启动、命令行参数、窗口状态、精确坐标、分屏槽位、置顶与透明度；
- 支持为任意工作区一键生成 **桌面快捷方式 (`.lnk`)** 双击直达启动；从多屏桌面切回单屏笔记本时，自动按比例折叠落位至主屏绝不丢窗，并配备轻量原生启动进度浮窗。

### 3. 窗口速切器 v2：自由拖缩 `Alt+Tab` 双栏中枢与独立同应用 `Alt+\``
- 增强 `Alt+Tab` 切换器升级为可自由掌控的双栏中枢，支持 **8 方向边角自由缩放、顶栏拖动定位、列表与预览分割线调宽，以及跨屏位置记忆**（拔掉外接屏时自动安全拉回主屏）；
- 新增 **桌面原位投影预览**、中文拼音首字母与模糊秒搜、完整窗口操作快捷键（`F4`–`F9` 窗口控制、`P` 置顶、`Z` 最大化/还原、`T` 收纳托盘及系统托盘窗捞回）；
- 新增 **独立同应用切窗（`Alt+\``）轨道**：即使不接管 Windows 原生 `Alt+Tab`，也可以单独开启 `Alt+\`` 在同一软件的多个窗口间极速切换。

### 4. 鼠标手势联动与全版本 Windows 轻量守护
- 在鼠标手势内置命令中新增 **6 项窗口管理专属动作**（打开双层布局管理器、启动/摆拍工作区、呼出窗口速切器、切换置顶与循环透明度），与手势流无缝衔接；
- 全链路兼容 **Windows 10 (1809+)、Windows 11、Windows Server 2019 / 2022 / 2025 及远程桌面（RDP）**，实测冷启动仅约 `50 ms`，闲置冷路径回收后物理工作集回落至 **`0.5 MB` 以内**。

---

## 升级方式

- **应用内一键升级（安装版与绿色便携版均支持）**：打开 `设置 -> 洞察与关于 -> 关于 -> 检查更新`（或点击系统托盘更新提示条）一键下载并自动完成原地升级，完整保留全部配置、剪贴板历史、固定选区与离线模型；
- **标准安装版手动覆盖**：下载下方 `Tools3000-v1.5.5-Setup.exe` 直接运行覆盖安装；
- **绿色便携版手动覆盖**：下载 `Tools3000-v1.5.5-win-x64-portable.zip` 解压覆盖程序目录（保留 `data/` 目录配置与数据）；
- **Scoop 用户**：在终端执行 `scoop update tools3000`。

---

### 文件完整性校验与安全查验 (SHA-256 / VirusTotal)

| 发布文件 | SHA-256 | 安全检测 |
| :--- | :--- | :--- |
| `Tools3000-v1.5.5-Setup.exe` | `99ffdfd31867f7d9547226d1f44b8b1bac038884048774ca1f448d3cdf2e96ff` | [VirusTotal 查验](https://www.virustotal.com/gui/file/99ffdfd31867f7d9547226d1f44b8b1bac038884048774ca1f448d3cdf2e96ff) |
| `Tools3000-v1.5.5-win-x64-portable.zip` | `fd3707dd631d1b248ea349341632a044c2e8b1a5eb18ee916e4bcb1edc6abb72` | [VirusTotal 查验](https://www.virustotal.com/gui/file/fd3707dd631d1b248ea349341632a044c2e8b1a5eb18ee916e4bcb1edc6abb72) |

下载后可在 PowerShell 中快速核对本地文件哈希：

```powershell
Get-FileHash .\Tools3000-v1.5.5-Setup.exe -Algorithm SHA256
```

> 如遇 Windows Defender SmartScreen 未知发布者提示，核对上述 SHA-256 与 VirusTotal 无误后点击「更多信息 -> 仍要运行」即可；安装包与便携包内均随附 SBOM.spdx.json 软件物料清单。
