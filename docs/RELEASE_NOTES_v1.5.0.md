# Tools3000 v1.5.0 Release Notes | 官方正式版发布说明

**[English](#english-release-highlights)** · **[简体中文说明](#本次重点更新与体验改进)** · **[Upgrade / 升级方式](#升级方式)**

---

## English Release Highlights

Welcome to **Tools3000 v1.5.0** (compared against **v1.4.17**). This feature milestone introduces two new opt-in productivity suites (Window Manager v1 and CapsLayer), multi-monitor fixed placement for Clipboard History, a four-layer fix for first-open Search black screens on Windows 10 LTSC / HDDs, upgraded Quick Translate with a native gesture command, and a redesigned Memory Policy overview and Changelog viewer:

1. **New Module: Window Manager Suite (v1 Initial Release — Preview Quality)** (#33, @LiangDongXing-cn)
   - *Honest note from the author*: **Window Manager is currently v1 (first playable release)**. Even the author is not fully satisfied with all the rough edges yet — it is usable enough for everyday window tiling and helpers today, and will be steadily refined and improved in future updates. All 8 sub-features default to **OFF** with zero background processes, hooks, or memory footprint until enabled.
   - Includes customizable **SnapZones** (with multi-monitor & per-Windows-virtual-desktop layout bindings, multi-zone spanning, and 1-click PowerToys FancyZones import), a **1:1 Fullscreen Native Visual Layout Editor** (`Win + Shift + ~`, supporting 1-click capture from real desktop windows and magnetic edge snapping to underlying apps), **Workspaces** launcher, **Quick Switch** window switcher, **Always on Top** & **Focus Border**, scroll-wheel **Opacity**, **Alt + Drag / Resize**, and **Multi-Monitor Rescue & Minimize to Tray**.
2. **New Module: CapsLayer Key Remapper (Opt-In, Zero Idle Overhead)**
   - Hold `CapsLock` to turn home-row keys into an instant navigation/editing layer (arrows, Home/End, PageUp/PageDown, Delete, media) while keeping quick tap and `Shift + CapsLock` native escape. Supports built-in presets, global & per-app profiles, and process exclusions.
3. **Clipboard History: Multi-Monitor Adaptive Placement & Fixed Position/Size** (#36, @HitomiXz)
   - Added 3 placement modes: **Follow Active Monitor** (default, centers cleanly on the monitor under your cursor), **Remember Last Position**, and **Fixed Position & Size** (supports negative coordinates for left/upper secondary monitors and an optional drag/resize lock).
   - Dimensions are persisted in DPI-independent logical units (DIP) so panel size stays visually consistent across 100% / 150% / 200% mixed-DPI setups, with automatic safe fallback to the primary monitor when an external display disconnects.
   - Added a 1-click **Fix Position & Size** target button directly on the clipboard title bar, synced live with `Settings -> Clipboard History -> Window Position & Size`.
4. **Search Window First-Open Black Screen Fix for Win10 LTSC / HDD** (#29, @xwshan)
   - Split all 8 locale dictionaries into on-demand single-language chunks, shrinking the main UI startup bundle by over **99%** (~1.94 MB -> ~11 KB) and loading the active surface in parallel to dramatically cut cold-start disk I/O on HDDs.
   - Upgraded background preload to silently warm up a real GPU first frame before first summon, removed premature ready timers and destructive first-frame SwapChain resets, and added a matching native search skeleton backdrop.
5. **Quick Translate (`Alt + X`) Interactive Language Bar & Native Translate Gesture**
   - Upgraded the popup header to `[Source] [Swap] [Target]` with auto-detect, 10 direct target languages, and persistent language memory; fixed manual target overrides, Simplified/Traditional Chinese conversion, and long-paragraph AI timeouts.
   - Added a native **Quick Translate** built-in command to Mouse Gestures and Hot Corners (default gesture: `L-R-D` / `←→↓` shaped like the letter `T`) with improved 180-degree turn recognition.
6. **Memory Policy Transparency Dashboard, Categorized Changelog & Expanded Tray Control Center**
   - Rebuilt the memory view into a pure read-only, zero-polling **Memory Policy** dashboard transparently documenting 20 built-in memory caps and reclamation strategies across 6 layers with interactive category filters.
   - Redesigned the offline **What's New** modal with natural-day timeline grouping, Feature / Improvement / Fix count pills and facet filtering, keyword highlighting, and compact/detailed density toggle.
   - Expanded the System Tray control grid to a balanced 12-tile (`3x4`) layout now including **Window Manager**, **CapsLayer**, and **QuickLook**, with crisp non-truncated labels.

**Upgrade**: In-app via `Settings -> Insights & About -> About -> Check for Updates` (supports both installer and portable builds), overwrite with `Tools3000-v1.5.0-Setup.exe` / `Tools3000-v1.5.0-win-x64-portable.zip`, or run `scoop update tools3000`.

---

## 本次重点更新与体验改进

欢迎升级至 **Tools3000 v1.5.0**。本版本以已正式发布的 **v1.4.17** 为对比基线，新增「窗口管理（第一版）」「Caps 引导改键」两大可选效率套件，解决剪贴板多屏定位与固定需求、深度根治 Win10 LTSC / 机械硬盘首次呼出搜索框黑屏，并带来划词翻译、内存策略与更新日志的多项核心升级：

### 1. 新增「窗口管理」套件（第一版尝鲜） (#33, @LiangDongXing-cn)
> **坦诚说明**：窗口管理功能目前为**第一版初始版本**，作者自己对不少细节体验也还不满意，目前日常基本分屏与窗口辅助已经可以凑合先用上，后续会专门找时间持续重构打磨，欢迎大家在 GitHub Issues 反馈建议。
- **全项默认关闭，不用即零占用**：独立原生轻量引擎，8 项子能力出厂默认全部关闭（不开启即 0 进程、0 钩子、0 MB 占用），按需开启；
- **自定义分屏吸附（SnapZones）**：内置多套经典网格/画布模板，支持多显示器与**按 Windows 虚拟桌面独立绑定不同布局**、按住 `Ctrl` 跨多区合并吸附、拖出分区自动还原原尺寸，并支持一键导入 PowerToys FancyZones 布局；
- **1:1 全屏原生可视化布局编辑器**：按 **`Win + Shift + ~`** 或在设置页一键唤起，直接在真实桌面 1:1 拖拽编辑网格与自由画布，支持**一键从当前桌面真实窗口生成布局（快捷键 `C`）**、拖拽自动磁吸底层真实软件窗口边缘与辅助参考线、60 步撤销及退出后冷路径自动释放内存；
- **全套窗口效率辅助**：同时提供工作区一键恢复（Workspaces）、窗口速切器（Quick Switch）、窗口置顶与聚焦边框、滚轮调节窗口透明度、`Alt` 任意位置拖动/缩放、多屏离屏救援与窗口收纳至系统托盘。

### 2. 新增「Caps 引导改键（CapsLayer）」模块
- **键盘双手不离主位**：按住 `CapsLock` 即可将常用键位映射为方向键、Home/End、翻页、删除或媒体控制；轻按可按偏好保持或切换大小写，随时按 `Shift + CapsLock` 原生切换大小写；
- **灵活定制与默认零开销**：内置多套高效预设，支持全局与分应用独立改键方案、运行窗口快速拾取排除，默认关闭零钩子零负担。

### 3. 剪贴板历史：彻底修复多屏错乱，新增位置与尺寸固定 (#36, @HitomiXz)
- **3 种多屏自适应定位模式**：支持「跟随当前显示器」（默认，自动在光标所在显示器居中弹出）、「记住上次位置与大小」、「固定屏幕位置与尺寸」（原生支持左侧/上方副屏负坐标）；
- **跨混合缩放零漂移**：按与 DPI 无关的逻辑尺寸保存宽高，在 100% / 150% / 200% 多屏间自动保持视觉大小一致；拔掉外接副屏时自动按相对位置安全回退至主屏可视区，插回副屏后自动恢复原固定位；
- **双入口便捷控制**：剪贴板浮窗标题栏新增一键「固定当前位置与大小」定位图标；设置页新增「窗口位置与尺寸（多屏自适应）」配置卡片，支持一键捕获当前面板位置、微调坐标与宽高、重置默认及开启「锁定位置与尺寸（防误拖动/缩放）」。

### 4. 深度根治 Win10 LTSC / 机械硬盘首次呼出搜索框黑屏问题 (#29, @xwshan)
- **首屏资源瘦身 99%+**：8 国语言词典改为按当前语种按需懒加载，入口主脚本从约 1.94 MB 缩减至约 11 KB，并与目标界面并行加载，大幅削减机械硬盘首次冷启动的磁盘读取与解析耗时；
- **后台无感真实首帧预热**：后台预载阶段现会隐身无感完成真实的 GPU 首帧渲染（任务栏与屏幕全程零闪烁、不抢焦点），用户首次按下 `Alt + Space` 即可直接呈现已就绪画面；
- **消除提前露黑框与原生骨架兜底**：移除过短的假就绪抢跑定时器与首次破坏性重绘刷新，同步内联首屏同主题底色并升级原生圆角搜索框骨架底衬。

### 5. `Alt + X` 原生划词翻译升级与「划词翻译」鼠标手势
- **交互式双语工具栏与记忆偏好**：浮窗顶栏升级为 `[源语种] [一键互译交换] [目标语种]`，支持自动检测与 10 语种独立直选并持久化记忆；修复手动切换目标语种被覆盖、简繁中文互译被跳过及单段长技术文本 AI 翻译偶发超时卡顿问题；
- **新增原生「划词翻译」手势与热角命令**：鼠标手势与触发角内置命令新增「划词翻译」，默认采用象形字母 `T` 轨迹（`←→↓`），同时加强 180 度折返手势识别准确度。

### 6. 「内存策略」全景透明看板、更新日志升级与托盘控制舱扩展
- **「内存策略」全景透明治理看板**：原性能监控页升级为纯只读、零后台轮询的「内存策略」看板，按 6 大层级透明展示全部 20 项内存上限封顶、按需零驻留与自动回收规则，支持按机制标签快速筛选；
- **离线更新日志弹窗体验重构**：升级为按自然日分组时间轴，清晰区分「新增功能 / 优化改进 / 问题修复」计数徽章与分面筛选，支持关键词搜索荧光高亮与简洁/详览切换；
- **托盘控制舱全面补齐**：系统托盘快捷开关升级为 12 格 `3x4` 对称布局 + 远控增强（正式纳入「窗口」「改键」「预览」三大模块），精炼短标消除文字截断。

---

## 升级方式

- **应用内一键升级（安装版与绿色便携版均支持）**：打开 `设置 -> 洞察与关于 -> 关于 -> 检查更新`（或点击系统托盘更新提示条）一键下载并自动完成原地升级，完整保留全部配置、剪贴板历史、固定选区与离线模型；
- **标准安装版手动覆盖**：下载下方 `Tools3000-v1.5.0-Setup.exe` 直接运行覆盖安装；
- **绿色便携版手动覆盖**：下载 `Tools3000-v1.5.0-win-x64-portable.zip` 解压覆盖程序目录（保留 `data/` 目录配置与数据）；
- **Scoop 用户**：在终端执行 `scoop update tools3000`。
