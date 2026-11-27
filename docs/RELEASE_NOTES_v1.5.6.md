# Tools3000 v1.5.6 Release Notes | 官方正式版发布说明

**[English](#english-release-highlights)** · **[简体中文说明](#本次重点更新与体验改进)** · **[Upgrade / 升级方式](#升级方式)**

---

## English Release Highlights

Welcome to **Tools3000 v1.5.6** (compared against **v1.5.5**). This release refines the **Window Manager** experience with a native Fluent UI and magnetic proportional zone canvas, introduces a standalone **Shortcuts Overview** page and a redesigned **OCR & Translation** workspace, adds **PowerPoint / slideshow exemption** to Fullscreen Do Not Disturb, keeps fresh installs lighter by default, and resolves two high-impact community-reported issues:

1. **Native Fluent Window Manager Polish & Magnetic Proportional Zone Canvas**
   - Upgraded **Quick Switch (`Alt+Tab`)**, the **Dual-Layer SnapZones Layout Hub**, and **Workspace capture/launch overlays** to a crisp native Fluent UI with instant light/dark and accent-color hot sync, zero blank frames across lock screens / RDP, and an ultra-responsive dedicated input pipeline with `1–9` direct switching and instant bottom search.
   - Always-on-top and focus borders now hide cleanly during drag/resize and snap flush on release with zero lag, zero ghost borders when apps minimize to tray, and proper Z-order below context menus and IME candidates.
   - Rebuilt the embedded **SnapZones Canvas Editor** around your display's true aspect ratio with pixel + percentage labels, translucent glass layering, 8-handle resizing, Figma-style magnetic snap guides (hold `Alt` to bypass), one-click tidy layout, duplicate zone, and one-click jump to the 1:1 fullscreen desktop editor.
   - Added a sticky interactive preview dock in the Workspace editor and **6 parameterized Window Manager mouse gesture commands** that can directly switch to a named SnapZones layout or launch a specific Workspace.
2. **Standalone Shortcuts Overview & Redesigned OCR / Translation Workspace**
   - Promoted **Shortcuts Overview** to a dedicated top-level menu under System Control with instant keyword filtering across all modules; refined Cloud Backup card layout and unified modal Z-index layering so the titlebar never overlaps dialogs.
   - Reorganized **OCR & Translation** into 3 clear tabs with a progressive 3-choice translation selector (**Free AI Translation**, **Free Fast Dual-Source**, or **Custom API / Local Ollama**), hiding developer fields for everyday users while keeping full custom provider flexibility and a built-in one-click test bar.
   - Updated offline OCR package guidance across all 8 languages based on real-world benchmarks (`PP-OCRv6` ~30 MB recommended pack for everyday screenshots, documents, and photos, plus an optional high-precision refinement pack).
3. **PPT Slideshow Exemption, Lighter Fresh Installs & Unified Naming**
   - Unified all related toggles under **Fullscreen Do Not Disturb** across 8 languages, and exempted **PowerPoint / WPS Presentation / LibreOffice Impress** slideshows so Keycast, Mouse Spotlight, Screenshot, Screen Recording, and global hotkeys remain fully available during presentations (while still stepping aside for native right-click slide menus).
   - Fresh installs now keep **Remote Boost**, **File Locksmith**, **Caps Layer**, **Window Manager**, and **Spacebar QuickLook** disabled by default for zero background overhead until opted in (upgrades preserve all existing user toggles), added clear input-habit guidance on Caps Layer, and streamlined the 8-language installer finish tips.
4. **Critical Community Bug Fixes**
   - Fixed an issue where pressing `Ctrl+Shift+Esc` failed to open Windows Task Manager (and could trigger bare `Esc` in WeChat or Chrome's browser task manager instead) (#38, @zmj159809).
   - Fixed Scoop installations failing to save configuration or failing to detect bundled binaries behind Scoop's `current` and `.tools3000` directory junctions — now works cleanly out of the box with no environment variable workarounds (#37, @zmj159809).

**Upgrade**: In-app via `Settings -> Insights & About -> About -> Check for Updates` (supports both installer and portable builds), overwrite with `Tools3000-v1.5.6-Setup.exe` / `Tools3000-v1.5.6-win-x64-portable.zip`, or run `scoop update tools3000`.

---

## 本次重点更新与体验改进

欢迎升级至 **Tools3000 v1.5.6**。本版本以已正式发布的 **v1.5.5** 为对比基线，带来窗口管理原生 Fluent 界面与等比磁吸画布升级、独立「快捷键一览」菜单、「文本识别与翻译」全新重构、PPT 全屏演示免打扰豁免、更克制的出厂默认配置，以及 Scoop 软链与 `Ctrl+Shift+Esc` 任务管理器快捷键两项高优先级问题根治：

### 1. 窗口管理原生 Fluent 视觉升级与等比磁吸画布
- **窗口速切器（`Alt+Tab`）、双层分屏布局管理器与工作区捕获/进度浮窗**全面升级为原生 Fluent 视觉体系，完美跟随系统浅色/深色主题与动态主题色实时热切换，锁屏与远程桌面场景稳定无黑屏；
- **速切器操控全面提速**：常驻极简底部检索栏、行内 `1–9` 数字键帽直达切窗、按住 `Alt` 直接键入字母筛选，彻底杜绝偶尔漏切回系统原生 `Alt+Tab`；
- **焦点边框与置顶边框零滞后零残影**：拖拽或缩放窗口瞬间隐身零拖影、松手严丝合缝贴合，窗口最小化或收入托盘时即时消失不留残影，且绝不遮挡右键菜单与输入法候选框；
- **分屏自由画布编辑器重塑**：告别宽屏压扁变形，升级为按当前显示器真实长宽比 1:1 居中呈现的通透画布，支持真实像素 + 百分比双标注、8 方向拖边缩放、磁吸对齐参考线（按住 `Alt` 临时关闭）、一键整理防重叠、复制区域以及右上角一键直达真实桌面 1:1 全屏编辑；
- **工作区吸顶预览与手势深度联动**：工作区编辑页支持微缩桌面舞台吸顶常驻联动高亮，并新增 **6 项参数化窗口管理手势命令**，画手势即可直接切换指定分屏布局或一键启动指定工作区。

### 2. 独立「快捷键一览」与「文本识别与翻译」全面重构
- **独立「快捷键一览」一级菜单**：将原深埋在通用设置里的快捷键面板升级为侧边栏「系统总控」固定第 3 菜单，内置关键词即时搜索，快速查改全站所有模块快捷键；同步优化云端备份卡片紧凑布局与全站弹窗层叠顺序，彻底解决标题栏遮挡更新日志/反馈弹窗的问题；
- **「文本识别与翻译」3 页签重构**：划词翻译升级为 **「免费 AI 翻译 / 免费双源极速 / 自有 API 与本地模型」** 3 选 1 场景卡片，默认零配置开箱即用、彻底隐藏开发者字段，极客仍可按需展开 OpenAI / DeepSeek / Gemini / DeepL / 本地 Ollama 完整配置，并在底部提供一屏可见即时测试条；
- **离线 OCR 引导基于实测精准分级**：`PP-OCRv6` 推荐包（约 30 MB）即可覆盖日常截图、文档表格、多语种与实拍照片，另提供按需下载的高精度精修包满足疑难细节识别需求。

### 3. 全屏免打扰支持 PPT 演示豁免、更克制的出厂默认配置
- **全屏免打扰统一命名与 PPT 放映豁免**：全站同类开关统一精简命名为「全屏免打扰」（8 语种同步）；PowerPoint、WPS 演示与 LibreOffice Impress 全屏放映时，**自动保留按键回显、鼠标聚光灯、截图、录屏与全局快捷键**，仅默认避让鼠标手势以保护原生右键翻页/激光笔菜单；
- **新安装更轻量无打扰**：全新安装默认不开启远控增强、文件占用解锁、Caps 引导改键、窗口管理与空格预览 5 项进阶模块（零后台占用，可在「设置 -> 功能模块」随时按需开启；覆盖升级完整保留老用户原有开关），并为 Caps 引导改键补充对输入法切换习惯的清晰提示；
- **安装完成页精修**：删减安装器冗余引导语，提供更简洁实用的 8 语种快速上手提示。

### 4. 关键社区反馈修复
- **修复 `Ctrl+Shift+Esc` 打开 Windows 任务管理器失效的问题** (#38, @zmj159809)：彻底修复带修饰键的 `Esc` 组合键被错误触发修饰键重置，导致无法唤起系统任务管理器、在微信误关窗或在 Chrome 误弹浏览器任务管理器的问题；
- **彻底修复 Scoop 安装后无法保存配置与识别不到内置组件的问题** (#37, @zmj159809)：在不降低任何路径安全防护的前提下，原生兼容 Scoop 的 `current` 与 `.tools3000` 双层目录联接，覆盖配置写入、插件加载、窗口管理、Picasa 看图器与搜索服务全部路径，同时保持开机自启跨版本平滑升级，无需手动配置环境变量。

---

## 升级方式

- **应用内一键升级（安装版与绿色便携版均支持）**：打开 `设置 -> 洞察与关于 -> 关于 -> 检查更新`（或点击系统托盘更新提示条）一键下载并自动完成原地升级，完整保留全部配置、剪贴板历史、固定选区与离线模型；
- **标准安装版手动覆盖**：下载下方 `Tools3000-v1.5.6-Setup.exe` 直接运行覆盖安装；
- **绿色便携版手动覆盖**：下载 `Tools3000-v1.5.6-win-x64-portable.zip` 解压覆盖程序目录（保留 `data/` 目录配置与数据）；
- **Scoop 用户**：在终端执行 `scoop update tools3000`。
