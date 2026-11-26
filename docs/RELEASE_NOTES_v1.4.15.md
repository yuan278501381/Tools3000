# Tools3000 v1.4.15 Release Notes | 官方正式版发布说明

**[English](#english-release-highlights)** · **[简体中文说明](#本次重点更新与体验改进)** · **[Upgrade / 升级方式](#升级方式)**

---

## English Release Highlights

Welcome to **Tools3000 v1.4.15** (compared against **v1.4.14**). This release introduces an offline in-app changelog modal after updates, eliminates the brief black flash when opening Search for the first time or resuming from idle, upgrades screenshot markup resizing precision and High-DPI resize cursors, defaults in-place screenshot translation to keyless Bilibili Index-Translate, and enhances dark-bar screen-photo OCR without regressing clean screenshot accuracy or speed:

1. **Offline "What's New" Changelog Dialog & Release History Browser (#31, @HitomiXz)**
   - Automatically displays a curated offline highlights dialog the first time Settings opens after an upgrade, showing a multi-version upgrade banner (e.g., `v1.4.11 -> v1.4.15`), `NEW` badges, exact local release dates and times, and relative time pills.
   - Fresh installs silently record the current version and never pop up an intrusive modal; upgrading across more than 3 versions automatically expands the latest 3 releases while collapsing earlier new entries to avoid overload.
   - Added a permanent **Changelog** button in `Settings -> Insights & About -> About` displaying the latest 10 releases per page with instant keyword/version search, `+10` pagination, and a direct link to GitHub Releases.
   - Added a two-way synced **Show changelog after updates** toggle in both the dialog footer and `Settings -> General`.
2. **Eliminated Brief Black Flash on First Search Open & Idle Resume (#29, @xwshan)**
   - Refined Search window presentation so the window stays atomically cloaked until the first visible UI frame is ready (both on first open after preload and when resuming from background suspend or minimize), removing the brief black frame reported on Windows 10 LTSC / HDD setups while keeping warm reopen instantaneous.
3. **Default In-Place Screenshot Translation to Keyless Bilibili Index-Translate**
   - In-place screenshot and pin translation now defaults out of the box to keyless **Bilibili Index-Translate (`Index-Translate-9B`)** for higher translation quality with zero API key setup.
   - If the public endpoint experiences temporary rate limiting or network disruption during everyday translation, Tools3000 automatically fails over to the free dual-channel fallback (Edge / Google), while **Test Connection** always reports the live status transparently.
4. **1:1 Responsive Screenshot Markup Resizing & High-DPI Vector Resize Cursors**
   - Fixed screenshot text boxes lagging, jumping, or feeling detached from the cursor while dragging corners/edges — resizing now tracks the pointer smoothly with continuous font scaling, locked opposite corners, and zero dead zones.
   - Eliminated cumulative drift and rounding erosion across all sibling markup tools (freehand pen, highlighter, rectangle corner radius, curved arrows, step numbers, and selection edges) by anchoring drag resizing to the press-time snapshot.
   - Replaced undersized legacy resize cursors on 150%+ High-DPI displays with crisp 4-axis vector resize cursors and DPI-scaled handle hit targets.
5. **Improved Screen-Photo Dark Error Bar OCR (Zero Regression on Existing Benchmarks)**
   - Enhanced offline OCR to recover full text from small, tilted, color-cast dark status/error bars photographed from monitors (such as ERP bottom message bars), while keeping clean digital screenshots on an untouched zero-overhead fast path.
6. **Refreshed About Page Tech Stack & 8-Language Polish**
   - Updated the **About -> Tech Stack** matrix to accurately reflect the current architecture (including Rust CLI & MCP Server, Rust Clipboard Kernel, MI-GAN 512 inpainting, WinHTTP/DPAPI translation gateway, `i18next`, `Vite 8.3`, and system read-only `WinSQLite3`) across all 8 languages.

**Upgrade**: In-app via `Settings -> Insights & About -> About -> Check for Updates` (supports both installer and portable builds), overwrite with `Tools3000-v1.4.15-Setup.exe` / `Tools3000-v1.4.15-win-x64-portable.zip`, or run `scoop update tools3000`.

---

## 本次重点更新与体验改进

欢迎升级至 **Tools3000 v1.4.15**。本版本以已正式发布的 **v1.4.14** 为对比基线，新增更新后离线更新日志弹窗与历史浏览、根治搜索框首次呼出与恢复瞬间的短暂黑屏闪烁、重塑截图全部标注工具缩放跟手度与高分屏矢量光标、将截图原位翻译默认切换为免密钥 Bilibili Index-Translate，并大幅增强手机拍屏暗色报错条的离线 OCR 识别能力：

### 1. 新增离线分页更新日志弹窗与版本历史浏览 (#31, @HitomiXz)
- **升级后自动聚焦展示亮点**：升级至新版本后首次打开设置，自动弹出纯离线「已更新至新版本」对话框；支持跨多版迁移聚合横幅、`本次新增` 微晶徽章、每版精确到分钟的本地发布时间（`YYYY-MM-DD HH:mm`）与相对时间标签，以及重大/功能/修复语义标识；跨越超过 3 个版本时自动仅展开最新 3 版防止长屏过载。
- **关于页常驻归档检索与分页**：在 `设置 -> 洞察与关于 -> 关于` 顶部新增「更新日志」常驻入口，默认首屏展示最新 10 个版本，支持按版本号或关键词即时检索全部内置历史版本，并可按 `+10` 逐页加载更早记录或一键跳转 GitHub Releases。
- **全新安装零打扰与偏好双向同步**：全新安装静默记录基线绝不打扰；弹窗左下角与 `设置 -> 通用设置` 同步提供「更新后自动显示更新日志」开关，可随时关闭或重新启用。

### 2. 消除搜索框首次呼出与休眠恢复瞬间的短暂黑屏闪烁 (#29, @xwshan)
- 针对首次呼出搜索框以及从后台挂起/最小化恢复瞬间偶发闪现短暂黑框的问题，升级为窗口级原子隐身热身与前端真实首帧就绪握手机制：等待界面完整绘制就绪后再瞬间显形（附超时兜底保护），常规二次呼出仍保持零等待秒开，显著改善 Win10 LTSC 及机械硬盘环境下的打开观感。

### 3. 截图原位翻译默认切换为免密钥 Bilibili Index-Translate
- 截图与贴图原位翻译现已开箱默认启用免密钥 **Bilibili Index-Translate (`Index-Translate-9B`)** 大模型通道，免去手动配置 API Key 即可获得更自然流畅的译文质量；
- 日常翻译如遇公网高峰限流或短暂网络波动，将自动无缝容灾回退至 Edge / Google 免费双源保障不中断；在设置页点击「测试连通性」时则直报当前所选通道的真实连通状态。

### 4. 标注截图缩放跟手度重塑与高分屏矢量缩放光标
- **文本框拉大缩小 1:1 丝滑跟手**：彻底修复截图文本框拖拽四角或四边缩放时框体不跟手、字号阶跃跳动及边界死区问题，实现连续平滑缩放与对侧角点零漂移锁定；
- **全家族标注工具与选区零漂移**：同步排查并修复自由画笔、荧光笔、矩形圆角、弯曲箭头、步骤序号及截图选区反复拖拽缩放时的累积圆整侵蚀与偏移问题；
- **高分屏高清矢量缩放光标与句柄**：针对 150%+ 高分屏缩放下系统默认缩放光标显得偏小一号的痛点，统一升级为 4 轴高清矢量双向缩放光标（自动适配屏幕缩放与系统光标大小设置），同步按 DPI 放大标注控制点与命中区，操作更省力精准。

### 5. 手机拍屏暗色报错条离线 OCR 识别增强（清晰截图与性能零回退）
- 针对手机拍摄显示器时底部倾斜、强色偏的暗底小字报错条（如 ERP 底部状态消息栏）容易漏字断句的痛点，新增仅在拍屏照片场景激活的暗底走廊补全与极性反色提纯通道，可完整读出整行暗底报错信息；
- 经全量 148 张基准图集实测验证，清晰数字桌面截图走零开销直通路径，整体识别字数、平均置信度与总耗时均保持正向提升、零回退。

### 6. 「关于 -> 技术栈」实证全面刷新（8 国语言全同步）
- 对照当前真实代码架构与依赖清单全面刷新技术栈展示：更新 `Vite 8.3` 与 `Rust Picasa Viewer (resvg 0.45)`，新增 **Rust CLI & MCP Server**、**Rust 剪贴板零依赖内核**、**MI-GAN 512 离线纹理修复**、**WinHTTP & DPAPI 原位翻译网关**、**quirc 离线二维码解码** 与 **i18next 8 语种国际化体系**，并将旧 SQLite 描述精确修正为系统原生只读 `WinSQLite3 & zlib`。

---

## 升级方式

- **应用内一键升级（安装版与绿色便携版均支持）**：打开 `设置 -> 洞察与关于 -> 关于 -> 检查更新`（或点击系统托盘更新提示条）一键下载并自动完成原地升级，完整保留全部配置、剪贴板历史、固定选区与离线模型；
- **标准安装版手动覆盖**：下载下方 `Tools3000-v1.4.15-Setup.exe` 直接运行覆盖安装；
- **绿色便携版手动覆盖**：下载 `Tools3000-v1.4.15-win-x64-portable.zip` 解压覆盖程序目录（保留 `data/` 目录配置与数据）；
- **Scoop 用户**：在终端执行 `scoop update tools3000`。
