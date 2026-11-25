# Tools3000 v1.4.13 Release Notes | 官方正式版发布说明

**[English](#english-release-highlights)** · **[简体中文说明](#本次重点更新与体验改进)** · **[Upgrade / 升级方式](#升级方式)**

---

## English Release Highlights

Welcome to **Tools3000 v1.4.13** (compared against **v1.4.12**). This release focuses on real-world daily friction reported by the community — delivering Listary-style configurable file dialog quick-jump across Explorer tabs and third-party file managers, IME-proof single-key shortcuts during capture/pin/studio editing, a root-cause fix for search window black-screen wakeups, a built-in keyless AI translation preset, and cleaner search & performance ergonomics:

1. **File Dialog Quick-Jump to Active File Manager & Configurable Dual Shortcuts** (`#30`, `@baowuji`; thanks to Linux.do `@sounder`)
   - Press `Ctrl+G` inside any standard Open/Save file dialog to jump straight to the active folder of your most recently used file manager tab (works even across multiple Windows 11 multi-tab Explorer windows or when the manager was just minimized).
   - Inline quick search inside file dialogs now uses `Ctrl+Shift+G` by default. Both shortcuts can be freely customized or cleared to disable under `Settings -> File Dialog Enhancer`, and both work even when the floating ribbon is turned off.
   - Broad third-party & non-standard compatibility: supports **Windows File Explorer**, **Total Commander**, **Directory Opus**, **XYplorer**, **Q-Dir**, **OneCommander**, **Files**, **Double Commander**, **FreeCommander**, and **MultiCommander**, plus explicit quick-jump support for **Qt** and **JetBrains** file dialogs (while keeping existing per-app folder memory intact).
2. **IME-Safe Single-Key Shortcuts Across Capture, Pin & Studio Canvases**
   - Prevents Chinese/Japanese/Korean IMEs from hijacking non-text single-key shortcuts (`R`, `Shift+R`, `C`, `E`, `S`, `Space`, `1-9`, etc.) or popping up stray candidate boxes during screenshot selection, screen pinning, and recording studio editing.
   - Strictly window-scoped: your global system IME language and Chinese/English state are never altered when leaving capture. Switching into text annotations, in-place pin text editing, or studio text fields restores IME smoothly at the caret.
3. **Fixed Search Window Black-Screen Reappearance on Hotkey Wakeup** (`#29`, `@xwshan`)
   - Resolved the race where hidden preloading and idle suspend could freeze the search surface before its first real visible frame, ensuring the search bar always renders crisply on hotkey summon.
4. **Built-In Keyless Bilibili Index-Translate Preset & Reasoning-Model Compatibility**
   - Keeps the default Zero-Key Dual Free channel (Edge / Google) untouched for instant low-latency translation.
   - Added a one-click **Bilibili Index-Translate (Free · Keyless)** preset under `Settings -> OCR & Translate -> In-Place & Quick Translate -> OpenAI-Compatible API`, with quick model switching between `Index-Translate-2B` (Fast), `Index-Translate-9B` (Balanced), and `Index-Translate-35B-A3B` (Flagship). Saved private API keys are automatically isolated and never sent to this public endpoint. Also strips `<think>` blocks globally for reasoning models.
5. **Zero-Warmup Offline Default Search Categories & Free Drag-to-Reorder for Document Content** (`#24`, `@clollipop`)
   - Summoning File Search when the default category is set to **Apps** or **Bookmarks** no longer wakes the background full-disk indexing service.
   - Unpinned **Document Content** on the search category bar (only **All** remains fixed in slot 1), allowing you to drag and reorder it anywhere freely (right-click the bar to restore default order).
6. **Streamlined Performance & Memory Page** (thanks to Linux.do `@zg2qxy`)
   - Removed the top live telemetry monitor and periodic polling card from `Settings -> Performance & Memory`, leaving a clean, read-only overview of the full-chain memory lifecycle and auto-recycle schedule.

**Upgrade**: In-app via `Settings -> Insights & About -> About -> Check for Updates` (supports both installer and portable builds), overwrite with `Tools3000-v1.4.13-Setup.exe` / `Tools3000-v1.4.13-win-x64-portable.zip`, or run `scoop update tools3000`.

---

## 本次重点更新与体验改进

欢迎升级至 **Tools3000 v1.4.13**。本版本以已正式发布的 **v1.4.12** 为对比基线，集中回应多项高频社区与 Issue 反馈，围绕 **文件对话框一键直达、截图/贴图/演播室输入法防吞键、搜索框黑屏根治、免密钥 AI 翻译预设与搜索/性能页体验打磨** 带来全面升级：

### 1. 文件对话框：一键直达活跃文件管理器目录 + 全可配置双快捷键（`#30`, `@baowuji`；感谢 Linux.do `@sounder`）
- **一键直达当前目录（默认 `Ctrl+G`）**：在系统的「打开 / 保存」文件对话框内按下快捷键，立即跳转至你最近使用的文件管理器当前激活标签页所在路径，省去层层翻找目录的麻烦；原有「按应用记住上次打开/保存目录」完整保留。
- **双快捷键全可自由配置，彻底脱离悬浮条限制**：内嵌快速搜索快捷键默认调整为 `Ctrl+Shift+G`；两项快捷键均可在 `设置 -> 文件对话框智能助手` 中独立自定义改键或清空禁用，即使关闭悬浮工具条也照常生效，且仅在文件对话框前台激活时生效，绝不干扰其他软件。
- **多窗口多标签精准定位 & 第三方管理器/非标准框兼容**：
  - 打开多个 Windows 11 多标签资源管理器窗口时，自动按最近使用顺序精准定位当前激活标签页（即使窗口刚刚最小化也能正确追溯）；
  - 除 Windows 文件资源管理器外，同步兼容 **Total Commander、Directory Opus、XYplorer、Q-Dir、OneCommander、Files、Double Commander、FreeCommander、MultiCommander**；
  - 显式按下快捷跳转键时，进一步安全兼容 **Qt** 与 **JetBrains** 系列软件的非标准文件对话框。

### 2. 截图 / 桌面贴图 / 录屏演播室：非文字输入态彻底免疫输入法吞快捷键
- **杜绝中/日/韩输入法拦截单键快捷键**：现在处于中文/日文/韩文输入法状态下，在截图选区、桌面贴图或录屏演播室里按 `R`、`Shift+R` 切换历史选区，或按 `C`、`E`、`S`、`Space`、`1~9` 等快捷键时，不再被输入法吞键，也不会弹出孤立的拼音候选框或把系统输入法中英文切乱。
- **严守窗口级隔离，零全局污染**：绝不篡改你的系统全局输入法状态，退出截图切回微信、浏览器或编辑器后输入法 100% 保持原样；点击文字标注、贴图原位改字或演播室文字输入框时瞬间自动恢复输入法并精准跟随光标。

### 3. 修复快捷键唤起文件搜索偶现黑屏的问题（`#29`, `@xwshan`）
- 彻底解决在后台预载与长时间闲置休眠恢复边界下，按快捷键呼出文件搜索框偶现黑屏无界面的问题，确保每次呼出均清晰稳定渲染。

### 4. 截图与划词翻译：新增 Bilibili Index-Translate（免费免密钥）一键预设
- **默认极速双源保持不变**：开箱默认仍为低延迟的免费双源（Edge / Google），兼顾高频截图瞬译速度。
- **一键免费体验高质量 AI 翻译**：在 `设置 -> 文字识别 -> 原位与划词翻译 -> OpenAI 兼容接口` 中新增 **Bilibili Index-Translate（免费免密钥）** 一键预设，无需登录与 API Key 即可使用，并支持快捷切换 `Index-Translate-2B`（极速）、`Index-Translate-9B`（均衡推荐）、`Index-Translate-35B-A3B`（旗舰）三档模型；切换至该免密钥预设时自动隔离你历史保存的私有密钥，绝不对外透传。
- **推理模型全面兼容**：全局支持自动剥离推理模型偶发输出的思考过程片段，使用 DeepSeek-R1、本地 Qwen3 等模型进行截图原位翻译更加稳定。

### 5. 文件搜索：纯离线默认分类零服务唤醒 & 「文档内容」自由拖拽排序（`#24`, `@clollipop`）
- **应用/书签默认分类零唤醒**：将搜索默认分类设为「应用启动」或「浏览器书签」后，呼出搜索框不会再提前拉起后台全盘索引服务，日常使用更轻更快。
- **分类栏更自由**：解除「文档内容」的默认固定位置限制，仅保留「全部」固定在首位，你可以将「文档内容」及其他分类自由拖拽排序（右键分类栏可随时恢复默认顺序）。

### 6. 性能与内存页：极简聚焦回收时间表（感谢 Linux.do `@zg2qxy`）
- 移除性能页顶部的实时资源监控面板与定时轮询，仅保留干净直观的「全链路内存生命周期与自动回收时间表」，界面更清爽、无额外观测负担。

---

## 升级方式

- **应用内一键升级（安装版与绿色便携版均支持）**：打开 `设置 -> 洞察与关于 -> 关于 -> 检查更新`（或点击系统托盘更新提示条）一键下载并自动完成原地升级，完整保留全部配置、剪贴板历史、固定选区与离线模型；
- **标准安装版手动覆盖**：下载下方 `Tools3000-v1.4.13-Setup.exe` 直接运行覆盖安装；
- **绿色便携版手动覆盖**：下载 `Tools3000-v1.4.13-win-x64-portable.zip` 解压覆盖程序目录（保留 `data/` 目录配置与数据）；
- **Scoop 用户**：在终端执行 `scoop update tools3000`。
