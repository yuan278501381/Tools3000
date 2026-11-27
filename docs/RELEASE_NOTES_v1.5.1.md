# Tools3000 v1.5.1 Release Notes | 官方正式版发布说明

**[English](#english-release-highlights)** · **[简体中文说明](#本次重点更新与体验改进)** · **[Upgrade / 升级方式](#升级方式)**

---

## English Release Highlights

Welcome to **Tools3000 v1.5.1** (compared against **v1.5.0**). This stability and responsiveness update eliminates first-screenshot clipboard lag, fixes wide-screen small-text OCR truncation, and hardens system-wide keyboard input responsiveness and clean process exit:

1. **Instant First-Screenshot Clipboard Delivery** (thanks to Linux.do @longqiuyu)
   - Fixed the noticeable delay between clicking Done on the first screenshot after launch and the image appearing in the clipboard.
   - Eliminated redundant PNG re-encoding across clipboard, auto-save, and screenshot history, moved stale temporary file cleanup off the UI thread, removed blocking disk flushes from auto-save and temporary clipboard files, and deferred post-capture memory trimming so the completion toast and clipboard paste feel instantaneous.
2. **Wide-Screen Small-Text OCR Accuracy Fix**
   - Fixed an issue where Windows built-in OCR aggressively downscaled wide desktop and ERP screenshots (such as 2K financial tables and dense tree lists) to 1680px width, causing fine small fonts to blur or drop characters.
   - Preserved full native clarity on 1080p / 2K / 4K screenshots and added gentle upscaling for medium-to-wide dense text screens, significantly improving recall on Windows OCR while keeping high-accuracy offline PP-OCRv6 models available as a 1-click download in Settings.
3. **Keyboard Input Responsiveness, Win11 Explorer Guard & Zero-Zombie Exit Hardening**
   - Isolated the global keyboard hook onto its own dedicated high-priority input thread with lock-free callback dispatch and merged Text Expander into the single shared keyboard hook bus, preventing UI activity or IME bursts from causing typing stutter.
   - Added a zero-overhead idle fast path for CapsLayer so everyday typing passes straight through untouched whenever CapsLock is not held.
   - Hardened QuickLook spacebar detection on Windows 11 File Explorer to safely avoid modern address bar and search box inputs.
   - Fixed an edge case where exiting Tools3000 could leave a windowless background process behind during final OS teardown, ensuring clean instant exit and smoother in-app updates.

**Upgrade**: In-app via `Settings -> Insights & About -> About -> Check for Updates` (supports both installer and portable builds), overwrite with `Tools3000-v1.5.1-Setup.exe` / `Tools3000-v1.5.1-win-x64-portable.zip`, or run `scoop update tools3000`.

---

## 本次重点更新与体验改进

欢迎升级至 **Tools3000 v1.5.1**。本版本以已正式发布的 **v1.5.0** 为对比基线，重点解决首次截图进剪贴板慢半拍、宽屏小字识字断断续续的问题，并对全局键盘输入响应与优雅退场做了底层稳定性加固：

### 1. 首次截图点完成秒进剪贴板（感谢 Linux.do @longqiuyu）
- **消除首帧复制卡顿**：重构截图完成后的编码与写盘链路，将原先在剪贴板、自动保存与历史记录间重复多次的无损编码合并为单次极速复用，同时将首次启动的历史临时文件清理迁至后台异步执行；
- **消除主线程阻塞与提示延迟**：免除自动保存与剪贴板临时文件写入时的同步物理刷盘等待，并将截图结束后的内存紧缩延后至静默冷态执行，点完成瞬间即可流畅弹出提示并秒速粘贴。

### 2. 修复宽屏小字桌面截图 OCR 漏字断识问题
- **解除宽图误压缩上限**：修复此前系统内置 Windows OCR 对 2K 及宽屏桌面/ERP 表单截图（如密集财务科目表）强行压缩至 1680px 宽度导致小号细体文字发糊、漏识的问题；
- **宽屏小字清晰度增强**：1080p / 2K / 4K 宽图不再误降采样，对中高密度桌面宽图执行温和清晰度增强，大幅提升 Windows OCR 召回率；如需极限小字与复杂表格准确率，也可在设置页一键下载离线高精度 PP-OCRv6 模型包。

### 3. 全局键盘响应加固、Win11 地址栏避让与彻底清场退出
- **独立高优先级单键盘输入通道**：将全局键盘监听迁至独立高优先级输入线程并实现无锁热路径分发，同时将文本扩展合并至同一条共享键盘监听总线（整套程序全局仅保留单一键盘钩子），杜绝界面忙碌时波及输入法打字流畅度；
- **Caps 引导改键日常打字零感知**：新增空闲态极速直通判断，未按住 `CapsLock` 时所有日常输入按键直接放行，零额外开销；
- **Win11 资源管理器空格预览避让加固**：补充对 Windows 11 现代地址栏与搜索框输入焦点的精准识别，避免在输入路径或检索时误触发空格预览；
- **彻底根治退出后偶发残留后台进程**：加固程序收尾退场逻辑，在完成全部配置保存、钩子卸载与资源释放后确保进程毫秒级干净清场，杜绝残留进程锁死安装目录影响覆盖升级。

---

## 升级方式

- **应用内一键升级（安装版与绿色便携版均支持）**：打开 `设置 -> 洞察与关于 -> 关于 -> 检查更新`（或点击系统托盘更新提示条）一键下载并自动完成原地升级，完整保留全部配置、剪贴板历史、固定选区与离线模型；
- **标准安装版手动覆盖**：下载下方 `Tools3000-v1.5.1-Setup.exe` 直接运行覆盖安装；
- **绿色便携版手动覆盖**：下载 `Tools3000-v1.5.1-win-x64-portable.zip` 解压覆盖程序目录（保留 `data/` 目录配置与数据）；
- **Scoop 用户**：在终端执行 `scoop update tools3000`。
