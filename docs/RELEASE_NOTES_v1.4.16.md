# Tools3000 v1.4.16 Release Notes | 官方正式版发布说明

**[English](#english-release-highlights)** · **[简体中文说明](#本次重点更新与体验改进)** · **[Upgrade / 升级方式](#升级方式)**

---

## English Release Highlights

Welcome to **Tools3000 v1.4.16** (compared against **v1.4.15**). This release introduces cinema-grade interactive smooth zoom on Double-Ctrl, per-hotkey foreground app auto-yield (defaulting `Alt+X` to yield to SSMS & Emacs), fixes the `Alt+X` quick-translate popup visibility & adds inline target-language switching, aligns Search `Ctrl+C` to copy real files, removes the stray Spacebar Quick Preview sidebar entry, polishes global 8-language copy & modular website experience, and hardens end-to-end diagnostic logging:

1. **Double-Ctrl Cinema Interactive Smooth Zoom (#34, @cainiaoNO1)**
   - Added two new **Double-Ctrl Trigger Modes** alongside the classic spotlight under `Settings -> Mouse FX & Highlights`: **Cinema Smooth Zoom** and **Cinema Smooth Zoom + Halo Ring**.
   - Delivers video-editor-grade smooth push/pull hardware zoom (zero overshoot, cursor-follow deadzone so clicking stays steady) while keeping native clicking, dragging, and typing 100% functional throughout the zoomed session.
   - Supports on-the-fly zoom adjustment (`Ctrl + Mouse Wheel`, `Ctrl + +/-`, `Ctrl + 0` reset) between `1.25x` and `4.0x`, instant exit via Double-Ctrl or `Esc`, and multi-layer automatic reset on lock screen, sleep, display change, or exit.
2. **Per-Hotkey Foreground App Auto-Yield (Zero Conflict in Pro Apps)**
   - Added fine-grained **Per-Hotkey App Auto-Yield** and a **Global Hotkey Do-Not-Disturb App List** under `Settings -> OCR & Translation` and `Settings -> General -> Keyboard Shortcuts`, with a one-click picker for currently running windows.
   - **Global Quick Translate (`Alt+X`)** now yields automatically out of the box when **SQL Server Management Studio (`ssms.exe`)** or **Emacs (`emacs.exe` / `runemacs.exe`)** is focused, letting `Alt+X` execute SQL or run `M-x` natively while keeping all other Tools3000 shortcuts active and restoring `Alt+X` instantly when you switch away.
3. **Fixed `Alt+X` Quick-Translate Popup Visibility & Added One-Click Language Switch**
   - Fixed the native `Alt+X` quick-translate popup appearing as an empty translucent frame and passing mouse clicks through on some Windows setups.
   - Added a clickable target-language direction badge right on the popup header so you can cycle translation targets (`AUTO -> ZH -> EN -> JA -> AUTO`) and re-translate in place without leaving your app.
4. **Search `Ctrl+C` Copies Files Directly & `Ctrl+Shift+C` Copies Full Paths**
   - Aligned `Alt+Space` Search shortcuts with Windows Explorer muscle memory: press `Ctrl+C` on any file or folder result to copy the file itself (ready to paste directly into folders or apps), and `Ctrl+Shift+C` to copy its full path.
   - Selecting text inside the search input still copies text normally, and non-file results (bookmarks / system commands) gracefully fall back to copying text.
5. **Cleaned Up Sidebar & Decoupled Spacebar Quick Preview Initialization**
   - Fixed the built-in **Spacebar Quick Preview** module erroneously appearing at the bottom of the Settings sidebar under Extensions (and falling back to General Settings when clicked).
   - Decoupled Spacebar Quick Preview startup from the File Search module so Spacebar Quick Preview works reliably even when File Search is disabled.
6. **Global 8-Language Copy Polish, Upgraded Onboarding & Modular Website**
   - Removed region-specific app mentions across all 8 UI languages, native dialogs, and Studio export presets for a clean international experience.
   - Updated the welcome onboarding wizard, sidebar subtitles, module descriptions, and official website to clearly highlight omni file/full-text/app/bookmark/command search, in-place & quick translation, `Ctrl+G` file dialog jump, cinema auto-zoom screen recording, Double-Ctrl smooth zoom, pro-app auto-bypass, E2E-encrypted cloud config sync, and independent per-module toggling.
7. **Cleaner End-to-End Diagnostic Logs**
   - Fixed cross-thread TraceID breaks in gesture execution and cinema zoom sessions, eliminated harmless shutdown/duplicate warnings, reduced routine gesture log noise while keeping full failure context, and added missing OS error codes for faster troubleshooting.

**Upgrade**: In-app via `Settings -> Insights & About -> About -> Check for Updates` (supports both installer and portable builds), overwrite with `Tools3000-v1.4.16-Setup.exe` / `Tools3000-v1.4.16-win-x64-portable.zip`, or run `scoop update tools3000`.

---

## 本次重点更新与体验改进

欢迎升级至 **Tools3000 v1.4.16**。本版本以已正式发布的 **v1.4.15** 为对比基线，带来 7 项重点功能进化与高频体验修复：

### 1. 双击 Ctrl 电影级交互平滑放大 (#34, @cainiaoNO1)
- **三模式自由选择**：在 `设置 -> 鼠标演示与特效` 中，除保留经典「**聚光灯高亮**」外，全新推出「**电影级平滑放大**」与「**电影级平滑放大 + 光环定位**」两种双击 `Ctrl` 模式（老用户升级后默认保持既有聚光灯习惯，可随时一键切换）。
- **电影推拉感与全程可操作兼得**：基于硬件全屏合成与临界阻尼平滑推拉曲线，放大期间**全程支持真实鼠标点击、拖拽、滚动与键盘打字**，告别传统静帧截屏放大无法操作的痛点；内置中心柔和死区跟焦，小范围点击选按钮时画面稳如泰山，光标移近边缘时镜头丝滑平移。
- **无级调倍与多重防卡死复位**：放大期间可随时用 `Ctrl + 滚轮`、`Ctrl + + / -` 无级调节倍率（`1.25x ~ 4.0x`），`Ctrl + 0` 一键回默认倍率；支持再次双击 `Ctrl` 或按 `Esc` 退出，并在锁屏、休眠、分辨率/显示器切换或程序退出时自动硬复位至 `1.0x`，闲置零占用。

### 2. 热键级「按前台应用自动让位」（专业软件零冲突）
- **单热键精准避让，绝不误伤其他功能**：新增「**单条快捷键按前台应用自动让位**」与「**全局热键免打扰应用名单**」（均支持一键从当前运行窗口拾取），当焦点切入指定专业软件时仅交还对应单条热键给目标程序原生响应，其余快捷键（如 `F1` 截图、`Alt+Space` 搜索）照常生效，切离窗口毫秒级自动恢复。
- **开箱自动避让 SSMS / Emacs**：全局划词翻译（默认 `Alt+X`）开箱默认避让 **SQL Server Management Studio (`ssms.exe`)** 与 **Emacs (`emacs.exe` / `runemacs.exe`)**，在 SSMS 中按 `Alt+X` 直接原生执行 SQL 零干扰；用户可随时在 `设置 -> 文本识别与翻译` 或 `设置 -> 通用设置 -> 快捷键总览` 增删避让列表。

### 3. 修复 `Alt+X` 划词翻译浮窗透明空框与点穿，新增一键切换语种
- 彻底修复部分环境下按下 `Alt+X` 弹出纯原生翻译浮窗时内容透明不可见、鼠标点击穿透的问题，恢复高对比清晰的即点即译体验；
- 浮窗顶栏新增**可点击语种方向胶囊**，点击即可在 `自动 -> 中文 -> 英文 -> 日文` 之间循环切换目标语种并就地重新翻译，无需切回设置页。

### 4. 搜索框 `Ctrl+C` 直接复制文件本体、`Ctrl+Shift+C` 复制路径
- 对齐系统资源管理器日常肌肉记忆：在 `Alt+Space` 搜索结果中按 `Ctrl+C` 直接将文件/文件夹本体复制到剪贴板（可直接粘贴到任意文件夹或应用），按 `Ctrl+Shift+C` 复制完整路径；
- 当搜索输入框内拖选了文字时，`Ctrl+C` 仍优先复制所选文字；书签与系统命令等非磁盘项自动降级复制文本。

### 5. 清除侧边栏「空格快速预览」异常菜单并解耦独立启动
- 修复 `设置` 侧边栏底部「扩展」区误出现「空格快速预览」幽灵入口且点击落入通用设置页的问题；
- 解耦「空格快速预览」与「文件搜索」模块的启动依赖，关闭文件搜索时空格快速预览开关依然独立生效。

### 6. 全软件 8 语种国际化文案润色、新手引导补全与官网双端升级
- **去地域割裂化**：统一清扫全库 8 国语言提示、右键菜单与演播室导出预设中的特定地域软件与竞品表述，搜索复制统一简洁提示「已复制文件到剪贴板」；
- **全触点亮点与加密云同步补齐**：新手引导（Step 0/1/3）、侧边栏副标题、功能模块列表、托盘提示与关于页同步补齐文件/全文/应用/书签/命令全能秒搜、多语离线识字与原位/划词翻译、文件对话框 `Ctrl+G` 快跳即搜、电影级自动运镜录屏、双击 `Ctrl` 交互平滑放大、专业软件自动避让及端到端加密配置云同步亮点；
- **官网 PC + 手机双端体验升级**：全面优化官网手机端与桌面端浏览体验，并明确呈现「每个功能均可个性化独立开关，关闭即卸载，零后台占用」的模块化理念。

### 7. 全链路诊断日志准确性与信噪比加固
- 修复手势后台执行与电影放大工作线程的 `TraceId` 断链问题，实现单次操作从触发到执行全程单链可追；
- 消除程序正常退出时连刷无害热键注销告警与设置窗关闭重复日志，收敛日常手势高频中间态日志至 `DEBUG` 级，同时为搜索管道与放大失败补齐系统错误码与多屏边界上下文，大幅提升排障效率。

---

## 升级方式

- **应用内一键升级（安装版与绿色便携版均支持）**：打开 `设置 -> 洞察与关于 -> 关于 -> 检查更新`（或点击系统托盘更新提示条）一键下载并自动完成原地升级，完整保留全部配置、剪贴板历史、固定选区与离线模型；
- **标准安装版手动覆盖**：下载下方 `Tools3000-v1.4.16-Setup.exe` 直接运行覆盖安装；
- **绿色便携版手动覆盖**：下载 `Tools3000-v1.4.16-win-x64-portable.zip` 解压覆盖程序目录（保留 `data/` 目录配置与数据）；
- **Scoop 用户**：在终端执行 `scoop update tools3000`。
