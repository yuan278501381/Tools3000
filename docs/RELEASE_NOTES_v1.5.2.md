# Tools3000 v1.5.2 Release Notes | 官方正式版发布说明

**[English](#english-release-highlights)** · **[简体中文说明](#本次重点更新与体验改进)** · **[Upgrade / 升级方式](#升级方式)**

---

## English Release Highlights

Welcome to **Tools3000 v1.5.2** (compared against **v1.5.1**). This major smoothness, compatibility, and responsiveness release eliminates visual frame drops across desktop effects and mouse gestures, fixes Windows 10/Server offline OCR and Save As freezes, introduces a resilient global mirror downloader, and adds real-time stage feedback for deep OCR and translation pipelines:

1. **Zero-Stutter Desktop Effects & Low-VRAM Mouse Gestures** (#33, @LiangDongXing-cn)
   - Rebuilt frame pacing and rendering across Mouse Spotlight, Halo, Click Ripples, Pointer Trails, Keycast, Toast Notifications, and Cinema Zoom with VSync-aligned scheduling for smooth 60Hz / 120Hz / 144Hz / 240Hz displays.
   - Added full automatic hardware-tier adaptation across Windows 10, Windows 11, Windows Server 2016–2025, Remote Desktop (RDP), legacy integrated GPUs, and software adapters without manual configuration.
   - Upgraded Mouse Gesture trails to a compact local dynamic canvas with non-spinning high-load pacing, staying fluid even under heavy CPU load while reducing active gesture VRAM usage by ~94% and releasing to zero when idle.
2. **Fixed Screenshot "Save As" Unresponsive Freeze** (thanks to Linux.do @Deepsea123)
   - Fixed an issue where clicking Save As during screenshot capture could cause the system file dialog to be trapped behind the topmost capture overlay, making Tools3000 appear frozen until force-closed.
   - The capture overlay now cleanly yields topmost focus before launching the isolated system Save As dialog, ensuring instant response and full compatibility with third-party Explorer shell extensions.
3. **Fixed Windows 10 / Server Offline OCR Always Returning "No Text Detected"** (thanks to Linux.do @Deepsea123)
   - Resolved a platform compatibility failure where users on Windows 10, Windows Server 2016–2025, or early Windows 11 builds without the newer system DirectML runtime received empty OCR results even after downloading all offline model packs.
   - Added a zero-bloat local CPU compatibility fallback engine that activates seamlessly when hardware acceleration is unavailable, delivering accurate offline PP-OCRv6 recognition across all supported Windows versions without increasing installer size.
4. **Global Dual-Stack Adaptive Model Downloader & Live Progress**
   - Replaced single-endpoint blocking downloads with a resilient multi-mirror downloader that races domestic and global mirrors automatically, detects stalls or slow connections within seconds, switches mirrors seamlessly with HTTP resume support, and cancels almost instantaneously.
   - Settings now displays transparent live download status including current stage, real-time speed, downloaded size, active mirror host, and automatic mirror switching notices.
5. **Real-Time Stage Feedback for Deep OCR & Translation Pipelines**
   - Everyday fast-path recognition and translation remain instant and unobtrusive; whenever a request enters a longer pipeline stage—such as cold-loading offline models, CPU compatibility depth mode, high-accuracy multi-line or low-confidence refinement, automatic script switching, batched translation, transient retry, automatic fallback provider switching, or AI background texture inpainting—the OCR Result window, Pinned Screenshot capsule, and Quick Translate popup (`Alt+X`) now display clean vector loading motion and exact live progress text.

**Upgrade**: In-app via `Settings -> Insights & About -> About -> Check for Updates` (supports both installer and portable builds), overwrite with `Tools3000-v1.5.2-Setup.exe` / `Tools3000-v1.5.2-win-x64-portable.zip`, or run `scoop update tools3000`.

---

## 本次重点更新与体验改进

欢迎升级至 **Tools3000 v1.5.2**。本版本以已正式发布的 **v1.5.1** 为对比基线，重点根治桌面特效与鼠标手势掉帧、截图另存为假死、Windows 10/Server 离线识字为空与模型包下载偶发卡死等高频痛点，并全面上线识字与翻译深度链路实时进度反馈：

### 1. 鼠标聚光灯、全系列桌面特效与鼠标手势高负载零掉帧升级 (#33, @LiangDongXing-cn)
- **全系列桌面特效告别掉帧**：全面重构鼠标聚光灯、光环、点击水波纹、移动流光轨迹、按键回显、Toast 气泡提示与焦点缩放的渲染节奏与垂直同步机制，丝滑适配 60Hz / 120Hz / 144Hz / 240Hz 高刷显示器；
- **全系统新老机型无缝自适应**：自动识别 Windows 10、Windows 11、Windows Server 2016–2025、远程桌面（RDP）以及早期核显与虚拟显卡环境，智能择优启用硬件合成或高兼容渲染管线，免手动配置；
- **鼠标手势高负载顺滑且显存直降 94%**：升级局部动态画布与防 CPU 抢占调度机制，CPU 满载时绘制手势依然跟手不卡顿，活跃期显存占用较旧版直降约 94%，静默后自动彻底归零释放。

### 2. 修复截图「另存为」程序无响应、强杀才弹出保存框的问题（感谢 Linux.do @Deepsea123）
- 彻底修复截图过程中点击「另存为」时，因系统文件保存对话框被截图置顶遮罩压住而导致界面看似卡死无响应、直到结束进程才弹出保存窗口的问题；
- 现在触发保存前截图选区会优雅让出置顶层级，系统保存对话框即点即弹，同时继续隔离第三方网盘或系统外壳扩展对主程序的干扰。

### 3. 彻底修复 Windows 10 / Server 离线识字永远提示「未识别到文字」的问题（感谢 Linux.do @Deepsea123）
- 根治在未预装新版系统 GPU 推理运行时的 Windows 10、Windows Server 2016–2025 及早期 Windows 11 系统中，即便已下载全部离线模型包、无论如何截图均提示「未识别到文字」的兼容性问题；
- 无需增加安装包体积，不支持系统 GPU 加速的环境会自动无缝切换至高兼容性本地深度识别模式并支持自动容错恢复，让全系列 Windows 系统均可开箱稳定使用离线高精度文字识别与原位翻译。

### 4. 全球双栈自适应离线模型下载引擎与透明进度显示
- **海内外镜像自动竞速与智能切源**：下载离线文字识别包、多语种扩展包与 AI 背景修复模型时，自动根据当前网络环境在中国大陆与全球镜像源之间快速择优；
- **防卡死看门狗、断点续传与秒级取消**：遇到单节点断流或龟速停滞时自动切换备用节点并支持断点续传，不再干等数分钟；点击取消时毫秒级响应中断；
- **下载状态全透明**：设置页离线引擎区实时显示当前阶段、实时网速、已下载体积、当前镜像节点及自动切源提示，下载进度一目了然。

### 5. 文字识别与翻译长耗时深度链路实时阶段反馈
- 常规快速识别与翻译依然保持零打扰瞬时出结果；
- 当任务进入离线引擎冷加载、老系统兼容深度识别、高精度/多行深度识字、低置信度精修、多语种扩展包自动复核，或翻译分批处理、网络波动自动重试、自动切换备用翻译源以及 AI 背景纹理修复等耗时环节时，识字结果窗、贴图等待胶囊与全局划词翻译浮窗（`Alt+X`）均会实时呈现清爽矢量加载动效与精确阶段说明，消除等待盲区。

---

## 升级方式

- **应用内一键升级（安装版与绿色便携版均支持）**：打开 `设置 -> 洞察与关于 -> 关于 -> 检查更新`（或点击系统托盘更新提示条）一键下载并自动完成原地升级，完整保留全部配置、剪贴板历史、固定选区与离线模型；
- **标准安装版手动覆盖**：下载下方 `Tools3000-v1.5.2-Setup.exe` 直接运行覆盖安装；
- **绿色便携版手动覆盖**：下载 `Tools3000-v1.5.2-win-x64-portable.zip` 解压覆盖程序目录（保留 `data/` 目录配置与数据）；
- **Scoop 用户**：在终端执行 `scoop update tools3000`。
