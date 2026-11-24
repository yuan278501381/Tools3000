# Tools3000 v1.4.6 Release Notes | 官方正式版发布说明

**[English](#english-release-highlights)** · **[简体中文说明](#本次重点新功能与体验升级)** · **[New Feature Paths / 新功能路径速查](#新功能入口与操作路径速查)** · **[SHA-256 & VirusTotal](#文件完整性校验与安全查验-sha-256--virustotal)**

---

## English Release Highlights

Welcome to **Tools3000 v1.4.6** (compared against **v1.4.5**). Shaped by community feedback across GitHub Issues, LINUX DO, and V2EX, this release brings **end-to-end encrypted configuration backup & portable in-place hot updates**, **lossless memory capping for dozens of 4K / scrolling desktop pins**, **color source app icons & universal drag-to-paste** across browsers, terminals, and AI IDEs, **15 dynamic macros & CJK fullwidth compatibility** in Text Expander, and a **re-organized 19-page Settings interface** with a dedicated **Performance & Memory** dashboard while trimming the idle main host down to **~2–6 MB** of private working set:

1. **Cloud & Sync-Folder Configuration Backup (Optional E2E Encryption) + Portable In-Place Hot Updates** *(#4, @Yafeiml)*
   - **Multi-Channel Config Backup**: Automatically detects local sync folders (OneDrive, Google Drive, Dropbox, iCloud Drive, Nutstore) and natively supports **WebDAV** and **S3-compatible object storage** (Cloudflare R2, AWS S3, Alibaba Cloud OSS, MinIO). Supports optional passphrase-based **AES-256-GCM end-to-end encryption**, automatic snapshot retention limits, optional silent backup on exit, and automatic local safety backups before any restore (`Settings -> System -> General -> Cloud & Sync Folder Backup`).
   - **Portable In-Place Hot Update**: Portable `.zip` users clicking `Settings -> Insights & About -> About -> Check for Updates` now update in place automatically while preserving `data/`, `config/`, `clipboard/`, and `models/` — never downloading `Setup.exe` or creating duplicate folders.
2. **Clipboard History (`Alt + V`): Real Color Source App Icons & Universal Smart Drag-Paste** *(thanks to LINUX DO @yue_bro, @宫灵紫凝)*
   - **Color Source App Icons**: Each clipboard card now displays the real color icon and friendly name of the source app (searchable by both friendly app name and `.exe` process name) *(thanks to LINUX DO @yue_bro)*.
   - **Universal Drag-to-Paste**: Smoothly drag text or rich snippets directly into **Chrome / Edge / Firefox web search inputs** (without triggering full-page "Drop files here" overlays, while still preserving native image/file drag-upload), **Windows Terminal / PowerShell / CLI TUIs / Git Bash**, **borderless Electron apps**, and **AI IDEs** (preserving `@Link` pills for single HTTP URLs while pasting plain text cleanly) *(thanks to Linux.do @宫灵紫凝)*.
3. **Desktop Pins & Capture (`F1` / `F3`): 4K / Long-Pin Memory Cap, Auto-OCR Toggle & Decoupled Finish Actions** *(thanks to LINUX DO @longqiuyu, @宫灵紫凝)*
   - **Lossless Memory Cap for Many 4K & Scrolling Pins**: Pinning a dozen 4K screenshots or tall scrolling captures no longer causes background OCR thread spikes or multi-GB RAM growth. Standard pins warm up Live Text silently, while 4K/scrolling pins prioritize instant recognition when hovered; idle, folded (`M` / double-click), or hidden (`Ctrl + Alt + Shift + H`) pins automatically enter **bit-exact lossless cold compression** (~33 MB 4K image compressed to ~1.8 MB in RAM, restored with zero quality loss when needed).
   - **New "Auto-Recognize Text When Pinning" Toggle**: Enabled by default in `Settings -> Productivity -> Capture & Record -> Pin & Fixed Regions` (synced with `OCR & Translation -> OCR & Interaction`). Turn off if you prefer strictly manual `T` / right-click OCR on pins.
   - **Decoupled Auto-Copy & Auto-Save on Capture Finish**: Turning off `Auto-Save to File` no longer pops up a forced Save-As dialog on `Enter` / `Finish`; when both toggles are off, `Finish` copies cleanly to the clipboard *(thanks to LINUX DO @longqiuyu)*. Also fixed occasional selection stutter after multi-day uptime *(thanks to Linux.do @宫灵紫凝)*.
4. **Text Expander Upgrade: CJK Fullwidth Compatibility, 15 Dynamic Macros & Modal Focus Fix** *(#25, @fxzou)*
   - **8-Language Input Ergonomics & 15 Macros**: Automatically treats CJK fullwidth characters and punctuation as equivalent to ASCII triggers by default (e.g. `；sig` matches `;sig`, configurable per snippet), adds a **Keep Trigger Key** toggle for boundary-triggered snippets, and expands dynamic macros to **15 variables** (`{{date+1d}}`/`{{date-7d}}` day offsets, localized `{{weekday}}`, compact/slash dates, `{{clipboard_trim}}`, `{{uuid}}`, `{{timestamp}}`, `{{user}}`, and `{{cursor}}`) with live cursor-aware preview (`Settings -> Productivity -> Text Expander`).
   - **Modal Input Focus Fix**: Fixed an issue where typing inside the New/Edit Snippet modal moved focus away after a single character, and prevented pressing `Esc` during CJK IME composition from closing modals *(#25, @fxzou)*.
5. **Re-Organized 19-Page Settings, Dedicated "Performance & Memory" Page & ~2–6 MB Idle Footprint**
   - **Ultra-Light Idle & Isolated File Dialogs**: Main host steady-state private working set is now down to **~2–6 MB** with zero hot-path latency trade-offs; system Open/Save file dialogs now run in short-lived isolated helper processes so third-party cloud-drive shell extensions never bloat the main app.
   - **Clearer Navigation & Transparency**: Top-level tabs on heavy pages (`Capture & Record`, `Mouse Gestures`, `OCR & Translation`, `File Search`), a compact searchable single-row `System -> Modules` list, a 4-domain `Global Shortcuts` overview in `General`, and a new **`Insights & About -> Performance & Memory`** page showing live metrics, one-click memory trim, and a transparent 10-rule automatic reclaim schedule.
6. **Search Window Grab-Drag, Reliable Foreground Open & 1-Paste Feedback Bundle**
   - Drag the `Alt + Space` Search window from any blank space on the top bar, category bar, or bottom status bar with a clear grab-hand cursor, and open files/folders reliably in front of other windows without taskbar flashing.
   - Clicking **Give Feedback** (`bottom-left of Settings sidebar`) now preserves the latest error/runtime log lines in pre-filled GitHub Issues and pre-loads the redacted `.zip` diagnostic bundle onto your clipboard so a single `Ctrl + V` in GitHub attaches it immediately.

| Feature / Setting | Where to Find It |
| :--- | :--- |
| **Cloud & Sync-Folder Backup** | `Settings -> System -> General -> Cloud & Sync Folder Backup` |
| **Auto-Recognize Text When Pinning** | `Settings -> Productivity -> Capture & Record -> [Pin & Fixed Regions] -> Auto-Recognize Text When Pinning` (default: On) |
| **Capture Auto-Copy / Auto-Save** | `Settings -> Productivity -> Capture & Record -> [Capture & Markup] -> Auto-Copy to Clipboard / Auto-Save to File` |
| **Text Expander 15 Macros & Preview** | `Settings -> Productivity -> Text Expander -> Add / Edit Snippet` |
| **Performance & Memory Dashboard** | `Settings -> Insights & About -> Performance & Memory` |
| **Modules Filter & 4-Domain Hotkeys** | `Settings -> System -> Modules` & `Settings -> System -> General -> Global Shortcuts` |
| **Search Window Drag & Portable Update** | Drag blank header/category/footer areas in `Alt + Space`; update portable builds in `Settings -> Insights & About -> About` |

**Upgrade**: In-app via `Settings -> Insights & About -> About -> Check for Updates` (works for both standard installer and portable builds), overwrite with `Tools3000-v1.4.6-Setup.exe` / `Tools3000-v1.4.6-win-x64-portable.zip`, or run `scoop update tools3000`.

---

欢迎升级至 **Tools3000 v1.4.6**。本版本以已正式发布的 **v1.4.5** 为对比基线，围绕 GitHub Issues、LINUX DO 与 V2EX 社区用户的真实使用反馈，带来了 **配置云端与同步盘加密备份**、**绿色便携版原地热更新**、**多张 4K 大图与长截图贴图无损内存封顶 + 贴图自动识字开关**、**剪贴板彩色来源应用图标与浏览器/终端全场景智能拖拽直贴**、**文本扩展器全半角兼容与 15 项动态变量**，同时重整了 **全站 19 个设置页面与专门的「性能与内存」看板**，并将主程序日常待机专用工作集进一步压降至 **约 2–6 MB**。

---

## 本次重点新功能与体验升级

### 1. 配置云端与同步盘备份（支持端到端加密）+ 绿色便携版原地热更新 (#4, @Yafeiml)
- **多通道配置安全备份与恢复**：新增配置备份工作台，自动识别本机已安装的 **OneDrive、Google Drive、Dropbox、iCloud Drive、坚果云** 同步目录，同时原生支持 **WebDAV** 与 **S3 兼容对象存储**（Cloudflare R2、阿里云 OSS、AWS S3、MinIO 等）。支持设置口令开启 **端到端加密**、自定义历史备份保留份数与退出软件时自动静默备份；每次恢复配置前均自动在本地生成一份恢复前保险副本，防止误覆盖。
  - **使用路径**：`设置 -> 系统总控 -> 通用设置 -> 配置云端与同步盘备份`
- **绿色便携版支持应用内一键原地热更新**：修复此前便携版在软件内点击更新会误下载安装包、导致产生两份程序目录的问题。现在便携版用户直接在 `设置 -> 洞察与关于 -> 关于 -> 检查更新` 点击升级，即可自动拉取便携包并在原目录完成热替换，完整保留 `data/`、`config/`、`clipboard/`、`models/` 等全部个人数据。

### 2. 剪贴板历史（`Alt + V`）：彩色来源软件图标 + 浏览器/终端/AI 工具全场景拖拽直贴（感谢 LINUX DO @yue_bro、@宫灵紫凝）
- **直观展示复制来源的彩色软件图标与名称**（感谢 LINUX DO @yue_bro）：每条剪贴板卡片顶部现直接显示来源软件的真实彩色图标与友好名称（悬停可查看完整 `.exe` 进程名），顶部搜索框同时支持按软件名称或进程名快速筛选。
- **打通全桌面复杂窗口的拖拽直贴体验**（感谢 Linux.do @宫灵紫凝）：
  - **现代浏览器网页搜索框零遮罩直贴**：将纯文本、代码或富文本拖入 Chrome、Edge、Firefox 网页输入框（如 Google 首页搜索框）时，不再误触发网页全屏「将文件拖放到此处」文件上传遮罩层，松手即可精准填入文字；而拖动图片或文件到浏览器时，依然完整保留原生以图搜图与拖拽上传文件体验；
  - **命令行终端与无边框软件兼容**：支持直接将命令文本拖入 `Windows Terminal`（PowerShell / 命令行交互界面）、`Git Bash` 以及 Clash Party 等无边框客户端输入框完成粘贴；
  - **桌面 AI 编程与聊天工具分流**：拖入 Cursor 等工具时，单条 `http(s)://` 网址保留原生链接卡片识别，普通多段文本则直接精准粘贴至光标处，不再被外层附件区静默吞没。

### 3. 桌面贴图与截图（`F1` / `F3`）：海量 4K 大图无损内存封顶、「贴图时自动识别文字」开关与截图完成动作解耦（感谢 LINUX DO @longqiuyu、@宫灵紫凝）
- **贴出十几张 4K 超大图或万像素长截图也不再暴涨内存**：
  - **智能队列与悬停优先识字**：常规尺寸贴图继续保持贴出即静默预热文字，划词零等待；面对多张 4K 全屏大图或超长滚动截图时，后台自动排队单张有序处理并在鼠标移入悬停或点击时立即优先秒级识别，彻底告别多图同时贴出时的瞬时卡顿与内存飙升；
  - **静置、折叠与隐藏态无损冷压缩**：后台静置贴图、双击或按 `M` 折叠为迷你胶囊的贴图，以及按 `Ctrl + Alt + Shift + H` 全局隐藏的贴图，会自动释放显卡全尺寸纹理并将原图无损压缩保存（单张 4K 贴图内存占用从约 `33 MB` 直降至约 `1.8 MB`），展开、标注、取色或保存时毫秒级逐像素无损还原，画质 100% 零损失。
- **新增「贴图时自动识别文字」独立开关（初始默认开启）**：
  - **使用路径**：`设置 -> 效率工具 -> 截图与录屏 -> [贴图与固定选区] 页签 -> 贴图时自动识别文字`（与 `效率工具 -> 文本识别与翻译 -> [识文与交互]` 实时同步）。若平时无需贴图自动识字可随时关闭；关闭后仍可在需要时对单张贴图按 `T`、右键选择提取文字或按 `Alt + T` 原位翻译。
- **截图「自动复制」与「自动保存」开关彻底解耦**（感谢 LINUX DO @longqiuyu）：关闭「自动保存到文件」后，按 `Enter` 或点击工具栏「完成」按钮不再强制弹出另存为窗口；当自动复制与自动保存同时关闭时，点击「完成」默认安静复制到剪贴板，工具栏完成提示也会随当前设置实时变化。

### 4. 文本扩展器全面进化：中日韩全半角自动兼容、保留触发键与 15 项动态宏
- **中文输入法全角符号无感触发**：默认自动兼容全角标点与字母（例如在中文输入法未切英文时直接敲 `；sig` 与 `;sig` 同样顺畅触发；若有特殊需求可在单条规则编辑窗单独勾选「区分全角与半角」）。
- **新增「保留输入的触发键」与 15 项动态变量胶囊**：按空格/回车/标点触发模式下可自由选择是否保留敲下的空格或标点；动态变量扩充至 **3 大类 15 项**——新增跨月/闰年精确计算的任意天数偏移（如 `{{date+1d}}`、`{{date-7d}}`）、跟随界面语言的本地化星期 `{{weekday}}`、紧凑/斜杠日期、去首尾空白剪贴板 `{{clipboard_trim}}`、随机 `{{uuid}}`、秒级时间戳 `{{timestamp}}` 与当前系统用户名 `{{user}}`，并在编辑弹窗底部内置带光标落点标记的**实时展开预览**。
  - **使用路径**：`设置 -> 效率工具 -> 文本扩展器 -> 新增短语 / 编辑`

### 5. 全站 19 页设置中心重整、全新「性能与内存」看板与 ~2–6 MB 极限轻量待机
- **主程序常驻待机压降至约 2–6 MB + 系统文件弹窗隔离**：在 100% 保持 1000Hz 跟手鼠标手势与 `Alt + Space` 瞬开速度的前提下，进一步回收冷启动、高分屏截图与剪贴板大图处理后的闲置堆空间；并将软件内全部「打开 / 另存为」系统文件选择框放入独立短命助手进程执行，彻底阻止第三方网盘右键图标扩展常驻寄生在主程序内吃掉数十 MB 内存。
- **新增专门一级页面「性能与内存」**：实时展示专用工作集、总工作集、CPU 及截图/手势/界面响应延迟，提供「立即静默回收」按钮与清晰的 **4 层 10 项全链路内存自动回收时间表**（可直接点击关联项跳转调整内存策略）。
  - **使用路径**：`设置 -> 洞察与关于 -> 性能与内存`
- **四大重型设置页签化、单行模块总控与快捷键四域分组**：
  - `截图与录屏`、`鼠标手势`、`文本识别与翻译`、`文件搜索` 顶部升级为清晰分类页签，配合左侧顶部搜索框（`Ctrl + K`）可一键直达任意子开关；
  - `设置 -> 系统总控 -> 功能模块` 升级为带关键词过滤与「全部 / 运行中 / 已停用」筛选的紧凑单行列表；
  - `设置 -> 系统总控 -> 通用设置 -> 全局快捷键` 告别按英文字母乱序排列，重新划分为「高频核心唤起 / 贴图与图文识别 / 屏幕录制控制 / 输入与辅助增强」四大直观分组。

### 6. 搜索窗口小手拖动与「问题反馈」一键 `Ctrl + V` 上传诊断包
- **搜索界面空白处抓手拖动**：按 `Alt + Space` 呼出搜索窗后，鼠标移至顶部搜索栏空白处、分类筛选栏空白处或底部状态栏即呈现清晰的小手图标，按住即可顺滑拖动窗口位置（双击顶栏空白处可切换最大化）。
- **问题反馈完整保留最新报错并支持一键 `Ctrl + V` 上传 `.zip`**：点击 `设置侧边栏左下角 -> 问题反馈` 跳转 GitHub 时，预填正文自动锚定最后发病时刻的最新运行轨迹与告警摘要，同时自动将本地脱敏 `.zip` 诊断包预置到系统剪贴板——在浏览器打开的 GitHub 输入框里直接按一次 `Ctrl + V` 即可上传附件。

---

## 重要问题修复

- **彻底修复文本扩展器新增/编辑短语弹窗每敲一个字自动失焦的问题** (#25, @fxzou)：修复在新增或编辑规则弹窗输入内容时每输入 1 个字符焦点就跳到右上角关闭按钮的问题，同时修复中文输入法候选词状态下按 `Esc` 误关闭整个弹窗的问题。
- **彻底修复长时间运行数天后屏幕截图框选掉帧卡顿及退出偶发残留进程**（感谢 Linux.do @宫灵紫凝）：精简后台冗余鼠标与窗口监听开销，优化截图遮罩高刷新率移动合成，并加固软件退出与升级重启清理流程。
- **修复从搜索界面打开文件或定位文件夹偶发不弹到最前面的问题**：解决打开已运行的单实例软件（如记事本、代码编辑器、资源管理器窗口）时偶发仅在任务栏闪烁橙色而不切至前台的痛点。

---

## 新功能入口与操作路径速查

| 功能与改进项 | 具体打开路径 / 操作方式 | 备注说明 |
| :--- | :--- | :--- |
| **配置云端与同步盘加密备份** | `设置 -> 系统总控 -> 通用设置 -> 配置云端与同步盘备份` | 自动识别主流同步盘，支持 WebDAV / S3 与可选端到端加密；恢复前自动留存本地副本 |
| **贴图时自动识别文字开关** | `设置 -> 效率工具 -> 截图与录屏 -> [贴图与固定选区] -> 贴图时自动识别文字` | 初始默认开启，与 `文本识别与翻译` 页同步；海量 4K 贴图自动无损冷压缩封顶内存 |
| **截图自动复制 / 自动保存解耦** | `设置 -> 效率工具 -> 截图与录屏 -> [截图与标注] -> 自动复制到剪贴板 / 自动保存到文件` | 关闭自动保存后按 `Enter` / 点「完成」不再弹另存为框 |
| **剪贴板彩色图标 & 全场景直拖** | 按 `Alt + V` 唤出剪贴板，按住任意条目直接拖入浏览器网页框 / 终端 / AI 聊天窗 | 顶部搜索框支持按中文软件名或 `.exe` 进程名筛选来源 |
| **文本扩展器 15 项宏 & 全半角兼容** | `设置 -> 效率工具 -> 文本扩展器 -> 新增短语 / 编辑` | 默认兼容中文输入法全角触发词，内置 `{{date+Nd}}` 等 15 项胶囊与实时展开预览 |
| **性能与内存透明看板** | `设置 -> 洞察与关于 -> 性能与内存` | 查看实时约 2–6 MB 主进程占用、响应延迟、一键回收与 10 项自动回收时间表 |
| **功能模块筛选 & 快捷键四域总览** | `设置 -> 系统总控 -> 功能模块` 及 `通用设置 -> 全局快捷键` | 支持状态筛选搜索、高频热键置顶与侧边栏 `Ctrl + K` 全局深跳 |
| **搜索窗抓手拖动 & 便携版热更新** | `Alt + Space` 顶栏/分类栏/底栏空白处按住拖动；便携版在 `设置 -> 关于 -> 检查更新` 升级 | 便携版原地热替换升级，完整保护 `data/` 与个人配置目录 |
| **问题反馈一键粘贴上传诊断包** | `设置窗口左下角 -> 问题反馈`（或 `设置 -> 洞察与关于 -> 关于 -> 问题反馈`） | 打开 GitHub 页面后在描述框按一次 `Ctrl + V` 即可上传本地脱敏 `.zip` 诊断包 |

---

## 升级方式

- **应用内一键升级（安装版与绿色便携版均支持）**：打开 `设置 -> 洞察与关于 -> 关于 -> 检查更新`（或点击系统托盘更新提示条）一键下载并自动完成原地升级，完整保留全部配置、剪贴板历史、固定选区与离线模型；
- **标准安装版手动覆盖**：下载下方 `Tools3000-v1.4.6-Setup.exe` 直接运行覆盖安装；
- **绿色便携版手动覆盖**：下载 `Tools3000-v1.4.6-win-x64-portable.zip` 解压覆盖程序目录（保留 `data/` 目录配置与数据）；
- **Scoop 用户**：在终端执行 `scoop update tools3000`。
