# Tools3000 v1.4.5 Release Notes | 官方正式版发布说明

**[English](#english-release-highlights)** · **[简体中文说明](#本次重点新功能与体验升级)** · **[SHA-256 & VirusTotal](#文件完整性校验与安全查验-sha-256--virustotal)**

---

## English Release Highlights

Welcome to **Tools3000 v1.4.5** (compared against **v1.4.4**). Shaped directly by community feedback, this release introduces a brand-new native **Text Expander** module, **drag-and-drop direct paste** in Clipboard History, a **Saved & Historical Capture Region Manager**, **120Hz+ hardware-composited pin zoom**, and unified navigation across Settings and Tray:

1. **New First-Class Module: Text Expander** *(thanks to Reddit r/windowsapps @redchrism, @iquizuanswer)*
   - Type short triggers (e.g. `;mail`, `;sig`, `;date`) in any app to expand static snippets or dynamic macros (date/time, clipboard reference, cursor placement) instantly.
   - Enabled out of the box with zero idle bloat (unhooks completely when disabled), automatic exclusion of password managers (KeePass, 1Password, Bitwarden, Enpass, LastPass, RoboForm, Dashlane) and RDP windows, zero clipboard history pollution, JSON import/export, and a live test sandbox in Settings.
2. **Clipboard History (`Alt + V`) Drag-Paste, Category Cleanliness & Active-App Tracking** *(thanks to Linux.do @宫灵紫凝)*
   - **Drag-to-Paste Without Closing**: Drag any clipboard card directly into editors, AI chat inputs, or browser address bars while keeping the clipboard panel open for consecutive drops.
   - **Hide Pinned Items in Other Tabs**: Optional toggle to keep pinned items exclusively inside the `Pinned` tab so they don't crowd `All / Text / Images / Files / Colors`.
   - **Live Target Tracking When Pinned**: Switching from App A to App B while the clipboard window is pinned now pastes accurately into App B.
   - **Fixes**: Resolved occasional multi-press `Alt + V` summon issues, delayed top-of-list refresh on newly copied items, and unresponsive click-to-paste after `Ctrl + A` or taking screenshots (`Esc` now exits hierarchically: edit -> clear multi-select -> close).
3. **Historical & Fixed Capture Region Manager** *(thanks to Linux.do @宫灵紫凝)*
   - Press `R` / `Shift + R` inside capture mode to cycle recent capture regions (configurable `0–50` persistent history & customizable cycle key), or press the new global shortcut **`Shift + F1` (`Repeat Last Capture`)** to jump straight into your previous region.
   - New **Region Manager** in `Settings -> Capture & Record`: pin fixed regions (never evicted), rename, fine-tune `X / Y / W / H` pixels, add centered presets (`1280×720`, `1920×1080`, `1080×1080`, `800×600`), or trigger a capture directly from any saved region.
4. **120Hz+ Silky-Smooth Desktop Pin Wheel Zoom (`F3`)**
   - Re-engineered wheel zoom with a GPU hardware-composited zoom stage, eliminating window resize jitter and edge flicker across high-DPI multi-monitor setups.
5. **Unified Update Badges, Non-Truncated 3-Section Tray & Consistent Menu Order**
   - Update notification dot now syncs across the system tray icon, tray top banner, and Settings `About` tab (with one-click `Ignore this version`); upgraded Tray Control Center to a rigid 3-section layout so bottom actions are never clipped; unified module ordering across Sidebar, Modules page, and Tray.

**Upgrade**: In-app via `Settings -> About -> Check for Updates`, overwrite with `Tools3000-v1.4.5-Setup.exe` / `Tools3000-v1.4.5-win-x64-portable.zip`, or run `scoop update tools3000`.

---

欢迎升级至 **Tools3000 v1.4.5**。本版本基于 **v1.4.4**，围绕社区用户的真实高频反馈带来了全新一级模块 **「文本扩展器（Text Expander）」**、**剪贴板拖拽直贴与置顶切窗跟随**、**截图历史选区记忆与固定选区管理工作台**、**桌面贴图电影级顺滑滚轮缩放**，以及 **三端更新红点联动、托盘三段式防截断重构与三端菜单有序重排**。

---

## 本次重点新功能与体验升级

### 1. 全新功能模块：文本扩展器（Text Expander）开箱即用（感谢 Reddit r/windowsapps @redchrism、@iquizuanswer）
- **输入短词秒出完整内容**：在浏览器、聊天软件、代码编辑器或办公文档的任意输入框中，敲下自定义的缩写触发词（如 `;mail`、`;sig`、`;date`），即可自动展开为常用长文本、邮件签名、地址信息或回复模板。模块默认开箱即用，不需要时可在「功能模块」或托盘面板一键关闭（关闭即释放全部钩子、零后台占用）。
- **静态短语与动态宏兼顾**：除最轻快的纯文本展开外，还支持自动填入当前日期/时间、引用当前剪贴板内容以及展开后将光标自动定位到指定位置；每条规则可自由选择「立即展开」或「敲空格/回车/标点触发」、是否区分大小写，并支持命中次数统计与 JSON 规则导入/导出。
- **默认隐私守护 + 页内实时体验**：自动避让 KeePass、1Password、Bitwarden、Enpass、LastPass、RoboForm、Dashlane 等主流密码管理器及远程桌面窗口；展开后自动还原原剪贴板内容，绝不污染剪贴板历史；设置页内置实时沙盒，配置完马上就能试打验证。

### 2. 剪贴板历史（`Alt + V`）交互全面进化：拖拽直贴、分类净化与跨窗跟随（感谢 Linux.do @宫灵紫凝）
- **拖拽直贴 + 连续多条拖放不关闭**：按住任意剪贴板条目卡片，可直接拖进外部软件输入框、代码编辑器、AI 聊天窗口或浏览器地址栏完成粘贴；拖放过程中及松手后剪贴板面板保持驻留不自动消失，方便连续拖入多条内容（全面适配常规软件、跨权限 Electron/AI 聊天窗、Sublime 选中后继续尾随拖入与 Chrome 地址栏）。
- **置顶条目支持从其他分类剥离（「隐藏置顶」开关）**：剪贴板工具栏与设置页同步新增「隐藏置顶」开关；开启后，已置顶的常用内容仅显示在「置顶」分类中，不再占据「全部 / 文本 / 图片 / 文件 / 颜色」等分类列表顶部空间。
- **置顶常驻时切换软件自动跟随粘贴**：将剪贴板固定在桌面上时，从软件 A 切换到软件 B 再点击粘贴，内容会自动精准贴入当前正在使用的软件 B 中，不再误贴回之前的软件 A。

### 3. 截图历史选区记忆与「固定选区管理工作台」（感谢 Linux.do @宫灵紫凝）
- **截图内一键复用历史框选范围**：进入截图后，按 `R` / `Shift + R` 即可快速调出并循环翻阅最近使用过的截图区域，写教程、做改版对比图或反复截取同一区域不再需要每次手动对齐。可在设置页自由调整历史保存数量（`0–50` 条，默认 `10` 条，关机重启持久保留）以及循环快捷键（`R`、`` ` ``、`F2`、`F4` 或关闭）。
- **新增全局快捷键「重复上次选区截图」（默认 `Shift + F1`）**：无需先开截图再按切换键，随时按下 `Shift + F1` 直接带着上一次的选区进入截图；该快捷键已在设置页与「全局快捷键」一览完整暴露，可自由改键。
- **全新「历史与固定选区管理工作台」**：在「设置 -> 截图与录屏」中可直观查看所有历史选区，将常用区域**置顶保存**（永不被自动淘汰）、自定义名称、精细编辑像素坐标与宽高（`X / Y / W / H`），一键添加 `1280×720`、`1920×1080`、`1080×1080`、`800×600` 等居中预设尺寸，并可直接点击任意选区立刻启动截图。

### 4. 桌面贴图（`F3`）升级 120Hz+ 电影级顺滑滚轮缩放
- 全面升级桌面贴图滚轮缩放体验，彻底消除缩放时的窗口抖动、边缘闪烁与掉帧卡顿，提供如 Picasa 看图器般自然跟手的高刷新率顺滑缩放手感（兼容多屏高分屏缩放与超长截图）。

### 5. 更新红点三端联动、托盘防截断与三端菜单有序重排
- **软件更新红点全链路直观可见**：检测到新版本时，系统托盘图标、托盘快捷面板顶部更新条与设置页侧边栏「关于」三处同步提示，点击即可直达更新详情、实时下载进度与一键安装；暂不想升级时可点击「忽略此版本」消除红点，下个新版本发布时再提醒。
- **系统托盘控制中心升级三段式防截断架构**：全新顶部吸顶标题栏 + 中部 3 列快捷磁贴独立滚动区 + 底部「管理员 / 退出」常驻吸底栏布局，兼顾美观紧凑与任意多模块扩展。
- **侧边栏、功能模块页与托盘三端顺序统一梳理**：按日常高频场景重新编排为四大清晰分区（视觉采集与剪贴板 -> 高频交互自动化 -> 文件检索与系统增强 -> 演示与远控），无论开关哪些模块，三端相对顺序始终保持严格一致。
- **新手引导与多语种说明同步更新**：更新 4 步新手引导，纳入文本扩展器、剪贴板历史、离线识字翻译等核心能力与实时自定义快捷键展示，并在全 8 国语言 README 中补充软件内「问题反馈」一键汇报路径。

---

## 重要问题修复

- **彻底修复剪贴板偶发连按多次才呼出、新复制内容未顶置与点击粘贴无响应**（感谢 Linux.do @宫灵紫凝）：修复焦点状态异常导致需要连按 2~3 次 `Alt + V` 才能唤出剪贴板、新复制内容未及时刷新置顶，以及刚截完图或按过 `Ctrl + A` 全选后点击粘贴毫无反应的问题；现在在剪贴板内按 `Esc` 按层级依次退出（退出编辑 -> 取消多选 -> 关闭窗口），交互更清晰可靠。
- **彻底修复托盘右键菜单偶发截断底部「退出」按钮**：消除弹出动画导致的高度误测问题，在各类分辨率与 DPI 缩放比例下均完整展示全部菜单与底栏操作。
- **修复全局快捷键列表个别词条漏译**：补齐「呼出/隐藏剪贴板」「重复上次选区截图」等快捷键在全部 8 国语言下的完整本地化显示。

---

## 新功能入口与快捷键速查

| 功能 | 在哪里打开 / 怎么用 | 说明 |
| :--- | :--- | :--- |
| **文本扩展器** | 设置：`效率工具 -> 文本扩展器`<br>开关：`功能模块` / `托盘快捷面板` | 默认开箱即用，可自定义触发词、动态宏、密码管理器避让并在页内实时试打 |
| **剪贴板拖拽直贴 & 隐藏置顶** | 按 `Alt + V` 唤出剪贴板，按住条目直接拖入目标输入框<br>点击工具栏「隐藏置顶」或在 `剪贴板历史` 设置页开关 | 拖放后剪贴板自动保持可见方便连贴；置顶常驻时切换软件自动跟随粘贴 |
| **历史与固定截图选区** | 全局直达：`Shift + F1`（重复上次选区截图）<br>截图内切换：`R` / `Shift + R`<br>管理工作台：`效率工具 -> 截图与录屏` | 支持置顶固定选区（永不淘汰）、命名、精确 `X/Y/W/H` 编辑、预设尺寸与按选区立即截图 |
| **更新提示与忽略版本** | 托盘图标红点 / 托盘顶部横幅 / 设置侧边栏「关于」 | 三端实时同步；点击「忽略此版本」后红点即刻消退 |

---

## 升级方式

- **应用内一键升级**：当前已安装 `v1.4.4` 的用户，可直接打开「设置 -> 关于 -> 检查更新」或点击托盘更新提示条一键下载并静默升级（保留全部配置、剪贴板历史、选区记录与离线模型）；
- **安装版手动覆盖**：下载下方 `Tools3000-v1.4.5-Setup.exe` 直接覆盖安装即可；
- **便携版用户**：下载 `Tools3000-v1.4.5-win-x64-portable.zip` 解压覆盖程序目录（保留 `data/` 目录配置与数据）；
- **Scoop 用户**：执行 `scoop update tools3000`。

---

### 文件完整性校验与安全查验 (SHA-256 / VirusTotal)

| 发布文件 | SHA-256 | 安全检测 |
| :--- | :--- | :--- |
| `Tools3000-v1.4.5-Setup.exe` | `5c82d2e860b914ffa2e1824e1805868e23df53a73b4c32ff23a362803a62d939` | [VirusTotal 查验](https://www.virustotal.com/gui/file/5c82d2e860b914ffa2e1824e1805868e23df53a73b4c32ff23a362803a62d939) |
| `Tools3000-v1.4.5-win-x64-portable.zip` | `be616bfd9113ef2b1a15aaa1ddbc12b1cb4dbebf66ace3c9d0c851c12cd965c8` | [VirusTotal 查验](https://www.virustotal.com/gui/file/be616bfd9113ef2b1a15aaa1ddbc12b1cb4dbebf66ace3c9d0c851c12cd965c8) |

下载后可在 PowerShell 中快速核对本地文件哈希：

```powershell
Get-FileHash .\Tools3000-v1.4.5-Setup.exe -Algorithm SHA256
```

> 如遇 Windows Defender SmartScreen 未知发布者提示，核对上述 SHA-256 与 VirusTotal 无误后点击「更多信息 -> 仍要运行」即可；安装包与便携包内均随附 SBOM.spdx.json 软件物料清单。
