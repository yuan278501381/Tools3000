# Tools3000 v1.4.8 Release Notes | 官方正式版发布说明

**[English](#english-release-highlights)** · **[简体中文说明](#本次重点更新与体验优化)** · **[Upgrade / 升级方式](#升级方式)** · **[SHA-256 & VirusTotal](#文件完整性校验与安全查验-sha-256--virustotal)**

---

## English Release Highlights

Welcome to **Tools3000 v1.4.8** (compared against **v1.4.7**). This release delivers a major offline OCR upgrade to **PP-OCRv6** with expanded multilingual support, smart auto-wrapping for screenshot text annotations, multi-path custom folder exclusions for File Search, a complete fix for Windows 10 LTSC black-screen and HDD indexing stalls, a non-intrusive clipboard delivery policy, and direct `Ctrl + C` -> `F3` image file pinning:

1. **Next-Gen Offline OCR (`PP-OCRv6`) & 65+ Language Ecosystem** (`#26`, `@NotAsea`)
   - **Faster & More Accurate Default Engine (`PP-OCRv6 Small`, ~30.3 MB)**: Covers 50 languages out of the box (Simplified/Traditional Chinese, English, Japanese, French, German, Spanish, Portuguese, and more) with noticeably higher accuracy and faster speed than the previous v4 pack. Existing users can upgrade in one click under `Settings -> OCR & Translate -> Offline Engine & Model Packs` with automatic cleanup of legacy model files.
   - **Optional High-Precision Refinement Pack (`PP-OCRv6 Rec Medium`, 73.1 MB)**: Smaller than the previous v4 refinement pack while delivering stronger recognition on complex/dense tables and code. Dual quality guardrails ensure everyday crisp screenshots stay instant with zero extra overhead; when slow-path refinement or auto language switching kicks in, a subtle live status indicator appears without interrupting your workflow.
   - **Refreshed On-Demand Language Packs & Expanded In-Place Translation**: Updated Apache-2.0 language packs for **Korean, Cyrillic (Russian), Arabic (with logical RTL order restoration), and Thai** (fixing multi-pack probe timeouts), plus expanded Windows OCR options (`fr-FR`, `de-DE`, `vi-VN`, `ar-SA`, `th-TH`). In-place screenshot translation now also recognizes Arabic, Thai, and Vietnamese scripts seamlessly.
2. **Smart Auto-Wrap for Screenshot Text Annotations** (Thanks to Linux.do `@zcx233`)
   - Typing or pasting text inside a screenshot now wraps cleanly at the selection boundary without inserting fake line breaks into your text—keeping English words intact, gracefully breaking long URLs, protecting CJK leading punctuation, and reflowing smoothly when you drag the text box or resize the selection. Smart edge nudging and bottom-edge lift prevent clipped text while keeping IME candidate windows and cursor selection aligned.
3. **File Search (`Alt + Space`): Multi-Path Custom Folder Exclusions** (Thanks to Linux.do `@sandxyz`)
   - Exclude one or many folders via the native folder picker, batch paste, or right-clicking any search result (`Exclude this folder` / `Exclude parent folder` with instant undo). Strict path-boundary matching (excluding `D:\Movies` never hides `D:\Movies2`) applies consistently across filename search, full-text content search, and the CLI / MCP interfaces.
4. **Windows 10 LTSC Search Black-Screen & Mechanical HDD High-I/O Fix** (Thanks to Linux.do `@zg2qxy`)
   - Automatically detects physical disk topology and rotational HDDs: partitions sharing the same mechanical drive are scanned sequentially at very-low background I/O priority, full-disk random-seek metadata pre-hydration is skipped on HDDs, and snapshot writes avoid per-chunk blocking flushes—eliminating 100% disk freezes.
   - Added a native state backdrop and automatic recovery for the Search window so cold startup or WebView2 issues never leave a blank black screen, while staggering service warmup until the UI finishes rendering.
5. **Non-Intrusive Clipboard Policy & `Ctrl + C` -> `F3` Image File Pinning**
   - **Zero Unwanted Clipboard Overwrites**: When **Auto-copy to clipboard** is turned off (even if **Auto-save to file** is also off), completing, saving, pinning, or translating a screenshot will never silently overwrite your existing clipboard—only explicit `Ctrl + C` / Copy writes to the clipboard.
   - **Copy Any Image File -> Press `F3` to Pin**: Press `Ctrl + C` on one or more image files in Windows File Explorer, Desktop, Tools3000 Search, Everything, or any file manager (supports PNG, JPG, WebP, BMP, TIFF, GIF first frame, ICO, HEIC, AVIF with transparency and EXIF orientation preserved), then press `F3` anywhere to pin them directly to your screen. Press `F3` repeatedly to step through multi-selected image files.

**Upgrade**: In-app via `Settings -> Insights & About -> About -> Check for Updates` (works for both standard installer and portable builds), overwrite with `Tools3000-v1.4.8-Setup.exe` / `Tools3000-v1.4.8-win-x64-portable.zip`, or run `scoop update tools3000`.

---

## 本次重点更新与体验优化

欢迎升级至 **Tools3000 v1.4.8**。本版本以已正式发布的 **v1.4.7** 为对比基线，带来离线文字识别引擎跨代升级至 PP-OCRv6、截图文字标注自动换行、文件搜索多路径排除、Win10 LTSC 与机械硬盘深度适配，以及更克制的剪贴板策略与图片文件快捷贴图：

### 1. 离线文字识别跨代升级至 `PP-OCRv6` 与 65+ 语种生态（`#26`, `@NotAsea`）
- **默认核心引擎升级为 `PP-OCRv6 Small`（约 30.3 MB）**：内置词表原生覆盖简/繁中文、英文、日文、法/德/西/葡等 50 种语言，复杂表格、多栏文档与高密截图识别准确度显著跃升，同时识别速度反而更快；已安装旧版 `PP-OCRv4` 的用户可在 `设置 -> 文本识别与翻译 -> 离线引擎与模型包` 一键无痛升级，升级完成后自动清理历史冗余模型文件；
- **可选高精度精修包同步升级为 `PP-OCRv6 Rec Medium`（73.1 MB）**：体积较旧版 v4 精修包更小，配合全新通用信噪比与几何保真双重护栏，常规清晰截图零额外耗时，仅对疑难低置信度行按需触发二阶精修；当触发精修或自动切换语种时，会在结果窗标题栏与提示条展示轻量进度反馈，快速识别全程静默零打扰；
- **按需扩展语种包与原位翻译全语种解锁**：更新纯 Apache-2.0 的 **韩语 / 俄语（西里尔）/ 阿拉伯语 / 泰语** 四大扩展包（修复全装 4 包时泰语偶发被跳过的问题，并补齐阿拉伯语 RTL 从右至左逻辑阅读序还原）；Windows 系统 OCR 同步新增法、德、越、阿、泰等语种选项；**原位翻译**同步解锁阿拉伯语、泰语及越南语变音文本识别，跨语种界面翻译开箱即用。

### 2. 截图文字标注遇边界自动换行（感谢 Linux.do `@zcx233`）
- 在截图内输入或粘贴长段文字时，触达选区右边界自动按词软换行（英文与数字单词不断腰、超长网址自动折断、中文避头标点保护），且**绝不在原文中注入假换行符**；
- 拖动文本框或拉宽/收窄选区时文字自动流式重排，靠近右/下边缘点击或放大字号触底时自动温和礼让上抬，中文输入法候选框、方向键换行与鼠标划词高亮全面对齐。

### 3. 文件搜索（`Alt + Space`）：自定义多路径排除文件夹（感谢 Linux.do `@sandxyz`）
- 支持在 `设置 -> 文件搜索` 与搜索框右上角「视图设置」中自由管理多个排除目录，提供原生系统文件夹选择器与多行/分号批量粘贴；
- 在搜索结果列表右键新增一键 **「排除此文件夹 / 排除所在文件夹」**，立即隐藏并支持 Toast 一键撤销；严格按目录边界精准屏蔽（例如排除 `D:\视频` 绝不误伤 `D:\视频素材`），同时覆盖文件名搜索、全文内容搜索与 CLI / MCP 接口。

### 4. 修复 Windows 10 LTSC 搜索黑屏与机械硬盘 100% 占用卡顿（感谢 Linux.do `@zg2qxy`）
- **机械硬盘物理拓扑感知与温和低优扫描**：自动识别物理磁盘与机械盘（HDD），同一块物理机械盘上的多个分区改为低优先级后台串行扫描（杜绝多分区并发寻道撕裂磁头），跳过机械盘全盘随机寻道预热并优化快照写入，彻底根治机械盘搜索卡死与磁盘占满问题；
- **消除冷启动搜索黑屏**：为搜索窗口补齐原生加载/状态界面与异常自动重载自愈能力，并将冷启动阶段的后台索引服务预热错峰延后至界面渲染就绪后执行，弹出更跟手、不再出现空白黑屏。

### 5. 更克制的截图剪贴板策略与「复制图片文件按 `F3` 直接贴图」
- **双关零污染剪贴板**：关闭「自动复制到剪贴板」后，无论「自动保存到文件」是否开启，点选完成、保存、贴图或原位翻译均 **绝不擅自覆写用户原有剪贴板**（仅主动按 `Ctrl + C` 或点击复制按钮时写入），看完翻译即走不再破坏原有剪贴板工作流；
- **图片文件 `Ctrl + C` -> `F3` 直接贴图**：在 Windows 资源管理器、桌面、Tools3000 搜索框、Everything、微信/QQ 或任意第三方文件管理器中 `Ctrl + C` 复制图片文件（支持 PNG / JPG / WebP / BMP / TIFF / GIF 首帧 / ICO / HEIC / AVIF，保留透明通道与照片正确方向），随时按下 `F3` 即可直接贴图到桌面；一次多选多张图片文件后，连续按 `F3` 可依次逐张贴出。

---

## 升级方式

- **应用内一键升级（安装版与绿色便携版均支持）**：打开 `设置 -> 洞察与关于 -> 关于 -> 检查更新`（或点击系统托盘更新提示条）一键下载并自动完成原地升级，完整保留全部配置、剪贴板历史、固定选区与离线模型；
- **标准安装版手动覆盖**：下载下方 `Tools3000-v1.4.8-Setup.exe` 直接运行覆盖安装；
- **绿色便携版手动覆盖**：下载 `Tools3000-v1.4.8-win-x64-portable.zip` 解压覆盖程序目录（保留 `data/` 目录配置与数据）；
- **Scoop 用户**：在终端执行 `scoop update tools3000`。
