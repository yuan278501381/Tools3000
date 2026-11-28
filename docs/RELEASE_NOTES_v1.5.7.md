# Tools3000 v1.5.7 Release Notes | 官方正式版发布说明

**[English](#english-release-highlights)** · **[简体中文说明](#本次重点更新与体验改进)** · **[Upgrade / 升级方式](#升级方式)**

---

## English Release Highlights

Welcome to **Tools3000 v1.5.7** (compared against **v1.5.6**). This release introduces **double-tap modifier global hotkeys** and an **in-capture `Ctrl+O` OCR shortcut**, polishes **Quick Switch (`Alt+Tab`)** and the **SnapZones Layout Editor**, shrinks installer and portable package sizes by ~20%+ while speeding up installation, cuts idle resident memory across all major modules, and hardens update checks against GitHub API rate limits:

1. **Double-Tap Modifier Hotkeys & In-Capture `Ctrl+O` OCR Shortcut** *(thanks to Linux.do @眠骨三生)*
   - **Double-Tap `Ctrl` / `Alt` / `Shift` Global Hotkeys**: You can now bind any global shortcut (such as Quick Search, Clipboard History, or Screenshot) to double-tapping `Ctrl`, `Alt`, or `Shift` right inside the shortcut recorder. Built with strict anti-false-trigger guards (ignores long holds, `Ctrl+C` / `Ctrl+Click` / `Ctrl+Wheel`, and key repeats) and automatically yields Mouse Spotlight's Double-Ctrl trigger with a clear notification to prevent dual firing.
   - **In-Capture `Ctrl+O` OCR Shortcut**: After selecting a region during a regular screenshot, press `Ctrl+O` (or `Ctrl+Shift+O`) to immediately run OCR text extraction on the current selection—right alongside `Ctrl+C`, `Ctrl+S`, `Ctrl+T`, and `Ctrl+Shift+T`, without conflicting with single-key `O` (Ellipse tool).
2. **Silky-Smooth Quick Switch (`Alt+Tab`) & Clear Drag Feedback**
   - **Reliable Hold-`Alt` + Repeated-`Tab` Switching**: Fixed cold-open initialization races, focus loss while holding `Alt`, and stale modifier states so holding `Alt` and pressing `Tab` / `Shift+Tab` stays 100% inside Tools3000 Quick Switch without leaking back to the native Windows switcher or getting stuck on the first row, and releasing `Alt` immediately activates the selected window.
   - **Hand Cursor Feedback on All Draggable Areas**: Hovering over blank margins, empty space below the window list, or the live preview pane now displays the standard hand cursor to clearly indicate where the window can be dragged; the center splitter hit zone has also been widened with a clear hover highlight.
3. **SnapZones Layout Editor Visual & Interaction Refinement**
   - **Single Active Focus in Free Canvas Mode**: Only the single zone directly under your cursor (or currently being dragged) is highlighted and brought to the top with its 8 resize handles, eliminating overlapping double-highlighted blue frames and cluttered handles.
   - **Unified Native Fluent Dividers in Grid Mode**: Removed distracting full-cell blue hover fills in Grid mode and unified existing dividers and new split guide lines into a single crisp Fluent bar matching your exact zone gap width, complete with magnetic snapping to classic fractions (25% / 33% / 50% / 67% / 75%) and live desktop window edges (hold `Alt` to temporarily disable snapping).
4. **Lighter Packages, Faster Upgrades & Deep Idle Memory Optimization**
   - **~20%+ Smaller Installer & Faster Extraction**: Switched binary packaging to exact PE import-closure pruning, removing ~22 MB of unreferenced third-party DLLs while keeping fast multi-threaded decompression; overwrite upgrades now automatically clean up legacy hashed UI assets and obsolete DLLs from previous installs.
   - **Balanced Three-Stage Idle Memory Governance**: Window Manager, Offline OCR, File Search, Tray, QuickLook, Settings, and Spotlight keep active sessions instant while automatically reclaiming large framebuffers, GPU DirectML sessions, and idle threads when unused.
5. **Rate-Limit Resilient In-App Update Checks**
   - Automatically falls back through zero-rate-limit official release metadata with strict SHA-256 verification when shared proxies hit GitHub API `403`/`429` limits, deduplicates update notifications, and clearly distinguishes rate-limit errors from network outages across all 8 languages.

**Upgrade**: In-app via `Settings -> Insights & About -> About -> Check for Updates` (supports both installer and portable builds), overwrite with `Tools3000-v1.5.7-Setup.exe` / `Tools3000-v1.5.7-win-x64-portable.zip`, or run `scoop update tools3000`.

---

## 本次重点更新与体验改进

欢迎升级至 **Tools3000 v1.5.7**。本版本以已正式发布的 **v1.5.6** 为对比基线，新增「双击修饰键全局快捷键」与「截图内 `Ctrl+O` 一键识字」、彻底打磨窗口速切器（`Alt+Tab`）与分屏编辑器细节、实现安装包零功能损失大幅瘦身、深度优化全链路闲置内存占用，并增强应用内检查更新的抗限流稳定性：

### 1. 全局快捷键支持双击修饰键 & 截图内 `Ctrl+O` 一键 OCR 识字（感谢 Linux.do @眠骨三生）
- **原生支持双击 `Ctrl` / `Alt` / `Shift` 全局快捷键**：在任意快捷键录入框中短时间内连按两次 `Ctrl`、`Alt` 或 `Shift` 即可绑定（适配 Listary / uTools / Everything 等高频操作习惯）；内置防长按、防 `Ctrl+C` / `Ctrl+点击` / `Ctrl+滚轮` 误触的多重护栏，且绑定「双击 `Ctrl`」时自动让位鼠标聚光灯的双击 `Ctrl` 触发并给出清晰提示，杜绝双触发冲突；
- **截图界面内新增 `Ctrl+O` 一键识别文字（OCR）**：常规截图拉完选区后，直接按 `Ctrl+O`（同时兼容 `Ctrl+Shift+O`）即可对当前选区立即发起文字识别，与 `Ctrl+C` 复制、`Ctrl+S` 保存、`Ctrl+T` 贴图、`Ctrl+Shift+T` 原位翻译完整对称，且不影响单键 `O` 绘制椭圆。

### 2. 窗口速切器（`Alt+Tab`）丝滑切窗与清晰拖动反馈
- **彻底解决按住 `Alt` 连按 `Tab` 漏回系统原版切屏与卡首项问题**：全面修复冷启/闲置后首次呼出异常、快速松键竞态、远程桌面/锁屏唤醒修饰键失步以及鼠标静止抢焦点的问题；现在按住 `Alt` 连续按 `Tab` / `Shift+Tab` 全程稳定停留在 Tools3000 速切界面内丝滑切换，松开 `Alt` 即刻切换目标窗口；
- **全拖动区域手型光标反馈**：鼠标移至窗口空白边框、左侧列表下方留白处以及右侧实时预览区域时，自动切换为清晰的手型光标提示「此处可拖动定位窗口」；同步拓宽中间分割线抓取热区并增加悬停高亮，左右栏比例拖拽更顺手。

### 3. 分屏布局编辑器（SnapZones）单一焦点与原版 Fluent 统一质感
- **自由画布模式单一激活焦点**：全画面任意时刻严格只高亮当前鼠标所在的唯一分区，并将该分区自动置顶显示 8 向缩放手柄，彻底消除多区重叠时同时高亮两个区域与手柄叠压的视觉干扰；
- **网格模式统一原版 Fluent 分割线与切割线**：取消普通悬停时的整块区域涂蓝，将已有分割线与新建切割线统一为与真实分区缝隙等宽的 Fluent 实心分隔条与纯净拖动手柄，支持常见等分比例（25% / 33% / 50% / 67% / 75%）与桌面已有窗口边缘磁吸对齐（按住 `Alt` 临时关闭磁吸）。

### 4. 安装包零损大幅瘦身 & 全链路闲置内存深度优化
- **安装包与便携包明显更小、覆盖安装更干净更快**：打包阶段自动精准裁除 20+ MB 未被引用的第三方冗余动态库，保持多线程高速解压的同时缩减下载与写入体积；覆盖升级会自动清理历代遗留的前端旧哈希缓存文件与过时库文件，大幅释放安装目录磁盘占用；
- **全链路三段式内存治理（热操作零卡顿、闲置及时回落）**：窗口管理引擎（WindowKit）、离线 OCR、文件搜索服务以及系统托盘/空格预览/设置窗口/鼠标聚光灯全面采用热保活与闲置冷回收策略，高频连续使用流畅不卡顿，闲置后自动释放大尺寸渲染缓冲、GPU 显存与空闲线程。

### 5. 应用内检查更新抗 GitHub 限流降级
- 当共享代理或频繁检测触发 GitHub API 限流（403 / 429）时，应用内「检查更新」自动无感切换至无速率限制的官方发布通道并强校验发布哈希，大幅提升更新成功率；
- 合并去重重复弹出的更新提示条，并在全部 8 种语言中精准区分「API 限流」与「网络连接异常」提示文案。

---

## 升级方式

- **应用内一键升级（安装版与绿色便携版均支持）**：打开 `设置 -> 洞察与关于 -> 关于 -> 检查更新`（或点击系统托盘更新提示条）一键下载并自动完成原地升级，完整保留全部配置、剪贴板历史、固定选区与离线模型；
- **标准安装版手动覆盖**：下载下方 `Tools3000-v1.5.7-Setup.exe` 直接运行覆盖安装；
- **绿色便携版手动覆盖**：下载 `Tools3000-v1.5.7-win-x64-portable.zip` 解压覆盖程序目录（保留 `data/` 目录配置与数据）；
- **Scoop 用户**：在终端执行 `scoop update tools3000`。

---

### 文件完整性校验与安全查验 (SHA-256 / VirusTotal)

| 发布文件 | SHA-256 | 安全检测 |
| :--- | :--- | :--- |
| `Tools3000-v1.5.7-Setup.exe` | `2004ddd5c588cab3dc3df2a77445105571a494ad1ce61e9bea7baa64dd325e1c` | [VirusTotal 查验](https://www.virustotal.com/gui/file/2004ddd5c588cab3dc3df2a77445105571a494ad1ce61e9bea7baa64dd325e1c) |
| `Tools3000-v1.5.7-win-x64-portable.zip` | `68660f2aec374db6b17a78df95577e5359e61b25b36cffa963e90e8df3bfc286` | [VirusTotal 查验](https://www.virustotal.com/gui/file/68660f2aec374db6b17a78df95577e5359e61b25b36cffa963e90e8df3bfc286) |

下载后可在 PowerShell 中快速核对本地文件哈希：

```powershell
Get-FileHash .\Tools3000-v1.5.7-Setup.exe -Algorithm SHA256
```

> 如遇 Windows Defender SmartScreen 未知发布者提示，核对上述 SHA-256 与 VirusTotal 无误后点击「更多信息 -> 仍要运行」即可；安装包与便携包内均随附 SBOM.spdx.json 软件物料清单。
