# Tools3000 v1.5.7 Release Notes | 官方正式版发布说明

**[English](#english-release-highlights)** · **[简体中文说明](#本次重点更新与体验改进)** · **[Upgrade / 升级方式](#升级方式)**

---

## English Release Highlights

Welcome to **Tools3000 v1.5.7** (compared against **v1.5.6**). This release focuses on polishing **Quick Switch (`Alt+Tab`)** and the **SnapZones Layout Editor**, cutting idle resident memory across all major background modules without sacrificing responsiveness, and making in-app update checks resilient against GitHub API rate limits:

1. **Silky-Smooth Quick Switch (`Alt+Tab`) & Clear Drag Feedback**
   - **Reliable Hold-`Alt` + Repeated-`Tab` Switching**: Fixed cold-open initialization races, focus loss while holding `Alt`, and stale modifier states so holding `Alt` and pressing `Tab` / `Shift+Tab` stays 100% inside Tools3000 Quick Switch without leaking back to the native Windows switcher or getting stuck on the first row, and releasing `Alt` immediately activates the selected window.
   - **Hand Cursor Feedback on All Draggable Areas**: Hovering over blank margins, empty space below the window list, or the live preview pane now displays the standard hand cursor to clearly indicate where the window can be dragged; the center splitter hit zone has also been widened with a clear hover highlight for effortless column resizing.
2. **SnapZones Layout Editor Visual & Interaction Refinement**
   - **Single Active Focus in Free Canvas Mode**: Only the single zone directly under your cursor (or currently being dragged) is highlighted and brought to the top with its 8 resize handles, eliminating overlapping double-highlighted blue frames and cluttered handles.
   - **Unified Native Fluent Dividers in Grid Mode**: Removed distracting full-cell blue hover fills in Grid mode and unified existing dividers and new split guide lines into a single crisp Fluent bar matching your exact zone gap width, complete with magnetic snapping to classic fractions (25% / 33% / 50% / 67% / 75%) and live desktop window edges (hold `Alt` to temporarily disable snapping).
3. **Deep Idle Memory Optimization Across All Modules (Zero Stutter on Active Use)**
   - **Window Manager, Offline OCR, File Search, Tray, QuickLook, Settings & Spotlight** now follow a balanced three-stage memory lifecycle: hot operations stay instant with zero premature eviction, while idle windows, large software framebuffers, GPU DirectML sessions, and background worker pools are automatically reclaimed and wake up seamlessly on demand.
4. **Rate-Limit Resilient In-App Update Checks**
   - When shared proxies or high request volumes trigger GitHub API `403`/`429` rate limits, **Check for Updates** automatically falls back through zero-rate-limit official release metadata with strict SHA-256 verification, deduplicates update notifications, and clearly distinguishes rate-limit errors from network outages across all 8 languages.

**Upgrade**: In-app via `Settings -> Insights & About -> About -> Check for Updates` (supports both installer and portable builds), overwrite with `Tools3000-v1.5.7-Setup.exe` / `Tools3000-v1.5.7-win-x64-portable.zip`, or run `scoop update tools3000`.

---

## 本次重点更新与体验改进

欢迎升级至 **Tools3000 v1.5.7**。本版本以已正式发布的 **v1.5.6** 为对比基线，重点打磨窗口速切器（`Alt+Tab`）操控手感与分屏编辑器视觉细节、深度优化全链路闲置内存占用，并全面增强应用内检查更新的抗限流稳定性：

### 1. 窗口速切器（`Alt+Tab`）丝滑切窗与清晰拖动反馈
- **彻底解决按住 `Alt` 连按 `Tab` 漏回系统原版切屏与卡首项问题**：全面修复冷启/闲置后首次呼出异常、快速松键竞态、远程桌面/锁屏唤醒修饰键失步以及鼠标静止抢焦点的问题；现在按住 `Alt` 连续按 `Tab` / `Shift+Tab` 全程稳定停留在 Tools3000 速切界面内丝滑切换，松开 `Alt` 即刻切换目标窗口；
- **全拖动区域手型光标反馈**：鼠标移至窗口空白边框、左侧列表下方留白处以及右侧实时预览区域时，自动切换为清晰的手型光标提示「此处可拖动定位窗口」；同步拓宽中间分割线抓取热区并增加悬停高亮，左右栏比例拖拽更顺手。

### 2. 分屏布局编辑器（SnapZones）单一焦点与原版 Fluent 统一质感
- **自由画布模式单一激活焦点**：全画面任意时刻严格只高亮当前鼠标所在的唯一分区，并将该分区自动置顶显示 8 向缩放手柄，彻底消除多区重叠时同时高亮两个区域与手柄叠压的视觉干扰；
- **网格模式统一原版 Fluent 分割线与切割线**：取消普通悬停时的整块区域涂蓝，将已有分割线与新建切割线统一为与真实分区缝隙等宽的 Fluent 实心分隔条与纯净拖动手柄，支持常见等分比例（25% / 33% / 50% / 67% / 75%）与桌面已有窗口边缘磁吸对齐（按住 `Alt` 临时关闭磁吸）。

### 3. 全链路闲置内存深度瘦身（热操作零卡顿、闲置及时回落）
- **窗口管理引擎（WindowKit）**：隐藏面板即刻释放窗口图标缓存，连续闲置后彻底回收软件渲染窗口池与工作集内存，闲置常驻内存大幅降低，再次呼出依然瞬间响应；
- **离线 OCR、文件搜索服务与桌面浮层**：离线 OCR 识别进程新增显存会话上限与自适应闲置退场，文件搜索服务精简常驻线程并在冷态启用轻量变更缓冲避免内存反复反弹，系统托盘、空格预览、设置窗口与鼠标聚光灯全面采用三段式保活与延时释放策略，高频连续使用时保持零卡顿、闲置后自动归还系统内存。

### 4. 应用内检查更新抗 GitHub 限流降级
- 当共享代理或频繁检测触发 GitHub API 限流（403 / 429）时，应用内「检查更新」自动无感切换至无速率限制的官方发布通道并强校验发布哈希，大幅提升更新成功率；
- 合并去重重复弹出的更新提示条，并在全部 8 种语言中精准区分「API 限流」与「网络连接异常」提示文案。

---

## 升级方式

- **应用内一键升级（安装版与绿色便携版均支持）**：打开 `设置 -> 洞察与关于 -> 关于 -> 检查更新`（或点击系统托盘更新提示条）一键下载并自动完成原地升级，完整保留全部配置、剪贴板历史、固定选区与离线模型；
- **标准安装版手动覆盖**：下载下方 `Tools3000-v1.5.7-Setup.exe` 直接运行覆盖安装；
- **绿色便携版手动覆盖**：下载 `Tools3000-v1.5.7-win-x64-portable.zip` 解压覆盖程序目录（保留 `data/` 目录配置与数据）；
- **Scoop 用户**：在终端执行 `scoop update tools3000`。
