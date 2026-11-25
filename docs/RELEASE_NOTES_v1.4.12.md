# Tools3000 v1.4.12 Release Notes | 官方正式版发布说明

**[English](#english-release-highlights)** · **[简体中文说明](#本次重点更新与体验改进)** · **[Upgrade / 升级方式](#升级方式)**

---

## English Release Highlights

Welcome to **Tools3000 v1.4.12** (compared against **v1.4.11**). This release introduces **9 customizable workflow behavior preferences** across File Search, Clipboard History, and Screen Pinning — keeping all existing defaults untouched while letting every user tune window dismissal and click actions to their own muscle memory:

1. **Customizable File Search (`Alt+Space`) Window Behavior** (thanks to Linux.do `@zg2qxy`)
   - Added 3 independent toggles under `Settings -> File Search` (all off by default to preserve current behavior):
     - **Auto-hide when focus is lost**: Turns File Search into a launcher-style pop-up that hides when you click outside, while intelligently staying open for pinned mode, QuickLook previews, context/system menus, and child dialogs.
     - **Auto-hide after opening a result**: Automatically hides the search window after successfully opening a file, folder, app, command, or bookmark (stays open when pinned or if opening fails).
     - **Clear last query on re-open**: Starts with a fresh empty search box after the window was hidden and summoned again (never clears when simply switching focus back to a visible window).
2. **Customizable Clipboard History (`Alt+V`) Behavior & Auto-Paste Fix**
   - Added 4 toggles under `Settings -> Clipboard` (preserving existing behavior by default):
     - **Auto-hide when focus is lost** (default: On)
     - **Auto-hide after pasting or copying** (default: On) — works seamlessly even when combined with *Auto-Paste* and *Auto-hide on blur*, keeping the panel floating safely while `Ctrl+V` is delivered so you can paste multiple items in a row.
     - **Keep panel open after drag-to-paste** (default: On) — turn off to dismiss automatically right after a successful drag delivery.
     - **Single-click item to paste immediately** (default: Off) — with built-in guards against double-paste, checkboxes, `Ctrl`/`Shift` multi-select, drag gestures, and inline editing.
   - **Fixed an Auto-Paste preference bug**: Double-clicking an item, pressing `Enter`, using `Alt+1..9`, or batch-merging items now strictly respects your **Auto-Paste** and **Plain Text by Default** settings instead of always forcing auto-paste.
3. **Customizable Screen Pinning (`F3`) Double-Click & Middle-Click Actions**
   - Added 2 action selectors under `Settings -> Capture & Pin` for users coming from Snipaste or other pinning tools:
     - **Double-click blank area**: Default `Annotate / Edit`, optional `Fold / Unfold` or `Close Pin`.
     - **Middle-click pin**: Default `Reset 1:1 Scale`, optional `Close Pin` or `Fold / Unfold`.
   - Built-in safeguards always unfold a folded pin first on double-click and block accidental closure while editing in-place replaced text.

**Upgrade**: In-app via `Settings -> Insights & About -> About -> Check for Updates` (supports both installer and portable builds), overwrite with `Tools3000-v1.4.12-Setup.exe` / `Tools3000-v1.4.12-win-x64-portable.zip`, or run `scoop update tools3000`.

---

## 本次重点更新与体验改进

欢迎升级至 **Tools3000 v1.4.12**。本版本以已正式发布的 **v1.4.11** 为对比基线，围绕高频效率场景中因人而异的操作习惯，为 **文件搜索、剪贴板历史、桌面贴图** 补齐 **9 项可自由定制的交互偏好**（默认 100% 沿用现有行为，老用户无任何学习成本），并修复了一处剪贴板自动粘贴设置未生效的问题：

### 1. 文件搜索 (`Alt+Space`)：自由切换「工具窗常驻」或「Launcher 即用即走」（感谢 Linux.do `@zg2qxy`）
在 `设置 -> 文件搜索` 中新增 3 项独立开关（默认均关闭，完整保留当前常驻工具窗体验）：
- **失焦时自动隐藏**：开启后点击窗口外部即自动收起；内置智能避让，置顶状态、按空格呼出 QuickLook 预览、弹出右键/系统菜单或子对话框期间绝不误关；
- **打开结果后自动隐藏**：开启后双击或按 `Enter` 打开文件、文件夹、应用、系统命令或书签成功后自动收起（置顶态或打开失败报错时保持展开）；
- **重新呼出时自动清空上次搜索词**：开启后每次隐藏窗口再按 `Alt+Space` 呼出时自动清空旧关键词，直接切回可见窗口焦点时绝不误清。

### 2. 剪贴板历史 (`Alt+V`)：全套窗口驻留与单击直贴定制 + 修复自动粘贴设置缺陷
在 `设置 -> 剪贴板` 中新增 4 项独立开关（默认 100% 保持当前行为），并同步修复一项既有设置生效问题：
- **失焦时自动隐藏**（默认开启）：关闭后可像独立工作窗一样常驻桌面，且自动持续记住最近使用的外部目标窗口；
- **粘贴或复制后自动隐藏**（默认开启）：关闭后支持连续多次粘贴不关窗，即便同时开启「自动粘贴」与「失焦自动隐藏」，在向目标窗发送 `Ctrl+V` 期间也会智能浮顶保护，避免被失焦隐藏误关；
- **拖拽直贴后保持窗口展开**（默认开启）：关闭后拖拽条目投递到目标应用成功即自动收起（置顶时不受影响）；
- **左键单击条目立即粘贴**（默认关闭）：追求极致少点一下时可一键开启，内置防双贴保护，且在勾选复选框、多选模式、`Ctrl`/`Shift` 多选、拖拽与编辑状态下自动豁免，绝不误触；
- **修复「自动粘贴」与「默认纯文本粘贴」设置未完全生效的问题**：现在无论双击条目、按 `Enter`、`Alt+1..9` 快捷编号还是多选批量合并粘贴，都会严格遵循你的设置（关闭「自动粘贴」后仅写入系统剪贴板而不再强行发送 `Ctrl+V`）。

### 3. 桌面贴图 (`F3`)：双击与鼠标中键动作随心定制，无缝兼容 Snipaste 肌肉记忆
在 `设置 -> 截图与贴图` 中新增 2 项动作选项：
- **双击贴图空白处**：默认 `进入编辑 / 标注`，可选 `折叠 / 展开贴图` 或 `关闭贴图`；
- **鼠标中键单击贴图**：默认 `恢复 1:1 原始大小`，可选 `关闭贴图` 或 `折叠 / 展开贴图`；
- 内置防误操作保护：处于折叠态时双击永远优先展开；原位改字编辑文字期间禁止双击或中键误关贴图；设为关闭/折叠时仍可正常左键拖选 OCR 文字。

### 4. 设置搜索与 8 国语言全面同步
- 以上 9 项新设置均已接入顶部全局设置搜索与简体中文、繁体中文、English、日本語、한국어、Русский、Português (Brasil)、Español 共 8 套语言界面。

---

## 升级方式

- **应用内一键升级（安装版与绿色便携版均支持）**：打开 `设置 -> 洞察与关于 -> 关于 -> 检查更新`（或点击系统托盘更新提示条）一键下载并自动完成原地升级，完整保留全部配置、剪贴板历史、固定选区与离线模型；
- **标准安装版手动覆盖**：下载下方 `Tools3000-v1.4.12-Setup.exe` 直接运行覆盖安装；
- **绿色便携版手动覆盖**：下载 `Tools3000-v1.4.12-win-x64-portable.zip` 解压覆盖程序目录（保留 `data/` 目录配置与数据）；
- **Scoop 用户**：在终端执行 `scoop update tools3000`。

---

### 文件完整性校验与安全查验 (SHA-256 / VirusTotal)

| 发布文件 | SHA-256 | 安全检测 |
| :--- | :--- | :--- |
| `Tools3000-v1.4.12-Setup.exe` | `f63d501d8ab02a3e0e5f7daefd68f871e1305d16345907679c2710610bf244bd` | [VirusTotal 查验](https://www.virustotal.com/gui/file/f63d501d8ab02a3e0e5f7daefd68f871e1305d16345907679c2710610bf244bd) |
| `Tools3000-v1.4.12-win-x64-portable.zip` | `6b1fa5d348a73798ee39f0811af8eceba6798e30dc6f745d3af58bf9c190ea01` | [VirusTotal 查验](https://www.virustotal.com/gui/file/6b1fa5d348a73798ee39f0811af8eceba6798e30dc6f745d3af58bf9c190ea01) |

下载后可在 PowerShell 中快速核对本地文件哈希：

```powershell
Get-FileHash .\Tools3000-v1.4.12-Setup.exe -Algorithm SHA256
```

> 如遇 Windows Defender SmartScreen 未知发布者提示，核对上述 SHA-256 与 VirusTotal 无误后点击「更多信息 -> 仍要运行」即可；安装包与便携包内均随附 SBOM.spdx.json 软件物料清单。
