# Tools3000 v1.4.15 Release Notes | 官方正式版发布说明

**[English](#english-release-highlights)** · **[简体中文说明](#本次重点更新与体验改进)** · **[Upgrade / 升级方式](#升级方式)**

---

## English Release Highlights

Welcome to **Tools3000 v1.4.15** (compared against **v1.4.14**). This release introduces an offline in-app changelog modal after updates, fixes Windows taskbar right-click menus, adds zero-latency right-click auto-bypass for CAD / UG / EDA / 3D apps, launches a native quick-translate popup for `Alt+X`, adds an independent Spacebar Quick Preview toggle, unifies file dialog shortcuts into the Shortcuts overview, eliminates the brief Search black flash, defaults in-place translation to keyless Bilibili Index-Translate, upgrades markup resizing & High-DPI cursors, and enhances dark-bar screen-photo OCR:

1. **Offline "What's New" Changelog Dialog & Release History Browser (#31, @HitomiXz)**
   - Automatically displays a curated offline highlights dialog the first time Settings opens after an upgrade, showing a multi-version upgrade banner (e.g., `v1.4.11 -> v1.4.15`), `NEW` badges, exact local release dates and times, and relative time pills.
   - Fresh installs silently record the current version and never pop up an intrusive modal; upgrading across more than 3 versions automatically expands the latest 3 releases while collapsing earlier new entries.
   - Added a permanent **Changelog** button in `Settings -> Insights & About -> About` displaying the latest 10 releases per page with instant keyword/version search, `+10` pagination, and a direct link to GitHub Releases.
   - Added a two-way synced **Show changelog after updates** toggle in both the dialog footer and `Settings -> General`.
2. **Eliminated Brief Black Flash on First Search Open & Idle Resume (#29, @xwshan)**
   - Refined Search window presentation so the window stays atomically cloaked until the first visible UI frame is ready (both on first open after preload and when resuming from background suspend or minimize), removing the brief black frame reported on Windows 10 LTSC / HDD setups while keeping warm reopen instantaneous.
3. **Fixed Windows Taskbar Right-Click Control Menus Not Opening (thanks LINUX DO @kidcode)**
   - Right-clicking app icons, system tray icons, the tray overflow flyout, or taskbar window thumbnails now passes through natively with zero interception by default, ensuring Jump Lists and window close/pin menus always open reliably on Windows 10 and Windows 11 (including standard non-admin sessions).
   - Existing users are automatically migrated on upgrade while keeping full flexibility: power users who want right-click gestures on the taskbar can still re-enable the **Right** trigger pill under `Settings -> Mouse Gestures -> Taskbar` at any time.
   - Hardened fallback click replay timing for modern WinUI 3 / XAML windows and made per-target trigger disable toggles take effect consistently across all profiles.
4. **Auto-Bypass Right-Click in CAD / UG / EDA / 3D Apps (Zero-Latency Passthrough)**
   - Added a built-in **Auto-Bypass Right-Click in CAD / 3D / EDA Apps** protection (enabled by default) covering 33 high-confidence right-click-intensive tools including AutoCAD, ZWCAD, GstarCAD, CAXA, Siemens NX (UG), SolidWorks, CATIA, Creo, Inventor, Fusion, Rhino, Revit, SketchUp, Altium Designer, EasyEDA Pro, KiCad, Blender, Maya, 3ds Max, Cinema 4D, Houdini, ZBrush, Unreal Engine, and Unity.
   - Only right-click inside the active CAD/3D window rectangle passes through natively with 0ms latency — middle-button, side-button, and secondary-monitor browser gestures continue working smoothly. The built-in list is transparently viewable under `Settings -> Mouse Gestures` and can be toggled off or overridden per app in Scope Targets.
5. **Native Quick-Translate Popup for Global Selection & Clipboard Translation (`Alt+X`)**
   - Fixed the global **Selection & Clipboard Translation** shortcut (`Alt+X`) failing to respond or opening an unreachable external web page.
   - Replaced it with a lightweight native Direct2D popup next to the cursor (zero extra browser process) that captures the current selection first and seamlessly falls back to clipboard text, powered by the unified translation engine (keyless Bilibili Index-Translate by default with Edge/Google failover), complete with pinning, dragging, scrolling, one-click `Ctrl+C` copy, retry, High-DPI scaling, and dark/light theme sync.
6. **Independent Toggle for Spacebar Quick Preview (#32, @powersee)**
   - Added a dedicated **Spacebar Quick Preview** module switch under `Settings -> Plugins -> File Search & System Utilities`.
   - Turning it off immediately passes Spacebar through at 0ms in the low-level keyboard hook and releases preview window memory, enabling seamless conflict-free coexistence with third-party preview tools such as QuickLook.
7. **Unified File Dialog Shortcuts (`Ctrl+G` / `Ctrl+Shift+G`) in General Shortcuts Overview**
   - Integrated **Quick Jump to Folder (`Ctrl+G`)** and **File Dialog Quick Search (`Ctrl+Shift+G`)** into `Settings -> General -> Keyboard Shortcuts` under a dedicated **Dialog Enhancer** section with a context badge (`Active in file dialogs`), two-way real-time sync with the Dialog Enhancer page, and zero global hotkey hijacking outside file dialogs.
8. **Default In-Place Screenshot Translation to Keyless Bilibili Index-Translate**
   - In-place screenshot and pin translation now defaults out of the box to keyless **Bilibili Index-Translate (`Index-Translate-9B`)** with automatic failover to the free Edge / Google channels if the public endpoint encounters temporary rate limiting or network disruption.
9. **1:1 Responsive Screenshot Markup Resizing & High-DPI Vector Resize Cursors**
   - Fixed screenshot text boxes lagging, jumping, or detaching from the cursor while resizing, eliminated cumulative drift across all sibling markup tools (pen, highlighter, rounded rectangles, curved arrows, step numbers, and selection edges), and replaced undersized legacy cursors on 150%+ High-DPI displays with crisp 4-axis vector resize cursors.
10. **Improved Screen-Photo Dark Error Bar OCR & Refreshed About Page Tech Stack**
    - Enhanced offline OCR to recover full text from small, tilted, color-cast dark status/error bars photographed from monitors without regressing clean digital screenshots or benchmark speed.
    - Updated the **About -> Tech Stack** matrix across all 8 languages to accurately reflect the current architecture (Rust CLI & MCP Server, Rust Clipboard Kernel, MI-GAN 512, WinHTTP/DPAPI gateway, `i18next`, `Vite 8.3`, and system read-only `WinSQLite3`).

**Upgrade**: In-app via `Settings -> Insights & About -> About -> Check for Updates` (supports both installer and portable builds), overwrite with `Tools3000-v1.4.15-Setup.exe` / `Tools3000-v1.4.15-win-x64-portable.zip`, or run `scoop update tools3000`.

---

## 本次重点更新与体验改进

欢迎升级至 **Tools3000 v1.4.15**。本版本以已正式发布的 **v1.4.14** 为对比基线，带来 10 项重点体验升级与社区反馈修复：

### 1. 新增离线分页更新日志弹窗与版本历史浏览 (#31, @HitomiXz)
- **升级后自动聚焦展示亮点**：升级至新版本后首次打开设置，自动弹出纯离线「已更新至新版本」对话框；支持跨多版迁移聚合横幅、`本次新增` 微晶徽章、每版精确到分钟的本地发布时间（`YYYY-MM-DD HH:mm`）与相对时间标签；跨越超过 3 个版本时自动仅展开最新 3 版防止长屏过载。
- **关于页常驻归档检索与分页**：在 `设置 -> 洞察与关于 -> 关于` 顶部新增「更新日志」常驻入口，默认首屏展示最新 10 个版本，支持按版本号或关键词即时检索全部内置历史版本，并可按 `+10` 逐页加载更早记录或一键跳转 GitHub Releases。
- **全新安装零打扰与偏好双向同步**：全新安装静默记录基线绝不打扰；弹窗左下角与 `设置 -> 通用设置` 同步提供「更新后自动显示更新日志」开关。

### 2. 消除搜索框首次呼出与休眠恢复瞬间的短暂黑屏闪烁 (#29, @xwshan)
- 升级为窗口级原子隐身热身与前端真实首帧就绪握手机制：等待界面完整绘制就绪后再瞬间显形（附超时兜底保护），常规二次呼出仍保持零等待秒开，显著改善 Win10 LTSC 及机械硬盘环境下的打开观感。

### 3. 修复运行后 Windows 任务栏右键控制菜单失效（感谢 LINUX DO @kidcode）
- **任务栏与托盘右键零延迟原生直通**：默认不再于任务栏区域拦截鼠标右键，确保在 Windows 10 / Windows 11 底部主任务栏、副屏任务栏、应用图标控制菜单（关闭窗口/固定/跳转列表）、系统托盘图标、隐藏托盘溢出区及任务栏悬停缩略图上右键点击 100% 稳定弹出原生菜单；
- **老用户升级自动生效 & 支持按需手开**：老版本升级后自动平滑迁移生效；如希望在任务栏使用右键手势，仍可随时在 `设置 -> 鼠标手势 -> 作用目标 -> 任务栏` 点亮「右键」触发药丸手动开启；
- **全目标独立触发器禁用与现代窗口回放加固**：打通全目标独立触发键禁用开关，并优化现代 WinUI 3 / XAML 窗口短按右键的真实按压节奏回放兼容性。

### 4. 专业 CAD / UG / EDA / 3D 软件右键自动避让（0ms 原生直通）
- 新增默认开启的 **「专业 CAD / 3D / EDA 软件右键自动避让」** 保护机制，内置涵盖 33 款高置信度右键密集专业软件清单（包括 AutoCAD、中望 CAD、浩辰 CAD、CAXA、Siemens NX / UG、SolidWorks、CATIA、Creo、Inventor、Fusion、Rhino、Revit、SketchUp、Altium Designer、立创 EDA 专业版、KiCad、Blender、Maya、3ds Max、Cinema 4D、Houdini、ZBrush、Unreal Engine、Unity 等）；
- 仅在对应专业软件窗口内对**右键**实施 0ms 原生放行（彻底消除拖转视角、平移画布与右键确认卡顿），中键、侧键以及光标移到副屏浏览器时的手势照常可用；在 `设置 -> 鼠标手势` 可随时展开查看完整清单、关闭总开关，或在「应用作用域」对任意单款软件单独覆盖开启。

### 5. 全新原生轻量划词与剪贴板翻译浮窗（修复 `Alt+X` 不生效）
- 彻底修复「全局划词与剪贴板翻译快捷键（默认 `Alt+X`）」触发无反应或误跳转不可达外部网页的问题；
- 全新打造零额外浏览器进程的纯原生轻量翻译浮窗：按下快捷键后优先抓取当前选区文字，未选中时无缝回退翻译剪贴板内容，1ms 在光标旁弹出加载卡片并直接复用内置翻译引擎（默认免密钥 Bilibili Index-Translate + Edge/Google 双源自动容灾），支持图钉固定、拖拽、长译文滚轮、`Ctrl+C` 一键复制译文、失败重试、高分屏缩放与深浅主题色实时同步。

### 6. 功能模块新增「空格快速预览」独立开关 (#32, @powersee)
- 在 `设置 -> 功能模块 -> 文件检索与系统增强` 中新增独立的 **「空格快速预览」** 开关（支持全局设置搜索直达）；
- 关闭后立即在底层键盘钩子 0ms 原生放行空格键并同步释放预览窗口内存，方便偏好使用第三方 QuickLook 等预览工具的用户零冲突共存，无需重启即可实时切换。

### 7. 文件对话框双快捷键纳入全局快捷键总览（单一事实源）
- 将文件对话框增强的 **「一键跳转当前目录 (`Ctrl+G`)」** 与 **「文件对话框内快捷搜索 (`Ctrl+Shift+G`)」** 统一纳入 `设置 -> 通用设置 -> 快捷键总览` 独立分组，标注「仅对话框生效」场景徽章，与文件对话框设置页双向实时同步，且在普通软件中绝不抢占全局 `Ctrl+G`。

### 8. 截图原位翻译默认切换为免密钥 Bilibili Index-Translate
- 截图与贴图原位翻译现已开箱默认启用免密钥 **Bilibili Index-Translate (`Index-Translate-9B`)** 大模型通道；日常翻译如遇公网高峰限流或短暂网络波动，将自动无缝容灾回退至 Edge / Google 免费双源保障不中断，设置页「测试连通性」直报所选通道真实状态。

### 9. 标注截图缩放跟手度重塑与高分屏矢量缩放光标
- 彻底修复截图文本框拖拽四角或四边缩放时框体不跟手、字号阶跃跳动及边界死区问题，同步消除画笔、荧光笔、矩形圆角、弯曲箭头、步骤序号及截图选区反复缩放时的累积漂移；
- 针对 150%+ 高分屏缩放下系统默认缩放光标偏小的问题，全面升级为 4 轴高清矢量双向缩放光标，同步按 DPI 放大标注控制点与命中区。

### 10. 手机拍屏暗色报错条离线 OCR 增强与「关于 -> 技术栈」实证刷新
- 增强手机拍摄显示器时底部倾斜、强色偏暗底小字报错条的离线 OCR 完整识别能力，经全量 148 图基准验证清晰数字截图零开销、零回退；
- 对照当前真实架构与依赖全面刷新 8 国语言「关于 -> 技术栈」卡片（新增 Rust CLI & MCP Server、Rust 剪贴板零依赖内核、MI-GAN 512 离线修复、WinHTTP & DPAPI 翻译网关、quirc 二维码解码、`i18next` 与 `Vite 8.3`，修正系统只读 `WinSQLite3 & zlib`）。

---

## 升级方式

- **应用内一键升级（安装版与绿色便携版均支持）**：打开 `设置 -> 洞察与关于 -> 关于 -> 检查更新`（或点击系统托盘更新提示条）一键下载并自动完成原地升级，完整保留全部配置、剪贴板历史、固定选区与离线模型；
- **标准安装版手动覆盖**：下载下方 `Tools3000-v1.4.15-Setup.exe` 直接运行覆盖安装；
- **绿色便携版手动覆盖**：下载 `Tools3000-v1.4.15-win-x64-portable.zip` 解压覆盖程序目录（保留 `data/` 目录配置与数据）；
- **Scoop 用户**：在终端执行 `scoop update tools3000`。
