# Tools3000 v1.5.3 Release Notes | 官方正式版发布说明

**[English](#english-release-highlights)** · **[简体中文说明](#本次重点更新与体验改进)** · **[Upgrade / 升级方式](#升级方式)**

---

## English Release Highlights

Welcome to **Tools3000 v1.5.3** (compared against **v1.5.2**). This urgent hotfix resolves a click-through regression introduced in `v1.5.2` when Mouse Click Ripples, Pointer Trails, or Mouse Gesture overlays are active, while preserving 100% of the `v1.5.2` zero-stutter performance and low-VRAM architecture:

1. **Fixed Mouse Clicks Being Blocked When Click Ripples or Pointer Trails Are Enabled**
   - Fixed a regression in `v1.5.2` where enabling **Show Click Ripple** or **Show Pointer Trail** caused the topmost effect overlay to intercept mouse button release and subsequent clicks across Settings, the system tray menu, and other applications.
   - Restored full cross-process mouse and touch click-through across both Mouse Effects and Mouse Gesture overlays while keeping the zero-stutter VSync hardware SwapChain and low-VRAM memory optimizations completely intact.
2. **Fixed Rapid Double-Clicks Cutting Off Click Ripple Animations**
   - Fixed an edge case where clicking a second time within the active ripple duration (~380 ms) prematurely dismissed the ripple overlay instead of rendering each rapid click cleanly.

**Upgrade**: In-app via `Settings -> Insights & About -> About -> Check for Updates` (supports both installer and portable builds), overwrite with `Tools3000-v1.5.3-Setup.exe` / `Tools3000-v1.5.3-win-x64-portable.zip`, or run `scoop update tools3000`.

---

## 本次重点更新与体验改进

欢迎升级至 **Tools3000 v1.5.3**。本版本以已正式发布的 **v1.5.2** 为对比基线，紧急修复了 `v1.5.2` 在开启鼠标点击光圈、鼠标轨迹或使用鼠标手势时可能拦截鼠标点击的问题，同时 100% 完整保留 `v1.5.2` 的零掉帧与低显存优化：

### 1. 紧急修复开启「显示点击光圈 / 鼠标轨迹」后鼠标左键被拦截失灵的问题
- 彻底修复 `v1.5.2` 开启鼠标点击光圈或鼠标流光轨迹后，透明特效浮层未对跨进程窗口完全穿透，导致设置页开关点不回去、系统托盘菜单及其他软件点击无响应的问题；
- 同步修复鼠标手势轨迹浮层在淡出瞬间可能拦截跨应用点击的同类隐患；
- 修复后 100% 完整保留 `v1.5.2` 的硬件垂直同步零掉帧渲染管线与低显存占用，不会引入任何掉帧回退。

### 2. 修复快速连点鼠标时点击光圈被提前中断的问题
- 修复在点击光圈播放期间（约 380ms 内）连续点击左键或双击时，前一次光圈被误清、第二次点击波纹未正常显示的问题，现在每次连点与双击均能清晰独立呈现。

---

## 升级方式

- **应用内一键升级（安装版与绿色便携版均支持）**：打开 `设置 -> 洞察与关于 -> 关于 -> 检查更新`（或点击系统托盘更新提示条）一键下载并自动完成原地升级，完整保留全部配置、剪贴板历史、固定选区与离线模型；
- **标准安装版手动覆盖**：下载下方 `Tools3000-v1.5.3-Setup.exe` 直接运行覆盖安装；
- **绿色便携版手动覆盖**：下载 `Tools3000-v1.5.3-win-x64-portable.zip` 解压覆盖程序目录（保留 `data/` 目录配置与数据）；
- **Scoop 用户**：在终端执行 `scoop update tools3000`。

---

### 文件完整性校验与安全查验 (SHA-256 / VirusTotal)

| 发布文件 | SHA-256 | 安全检测 |
| :--- | :--- | :--- |
| `Tools3000-v1.5.3-Setup.exe` | `8a5fa947ae0eb7e573274afd9828efdb221e40b12e6fbdfb406529a43427203f` | [VirusTotal 查验](https://www.virustotal.com/gui/file/8a5fa947ae0eb7e573274afd9828efdb221e40b12e6fbdfb406529a43427203f) |
| `Tools3000-v1.5.3-win-x64-portable.zip` | `25629c183d37132700e4f9be04fba77755cdd0383fc7a2b2c7372e25e1869aa3` | [VirusTotal 查验](https://www.virustotal.com/gui/file/25629c183d37132700e4f9be04fba77755cdd0383fc7a2b2c7372e25e1869aa3) |

下载后可在 PowerShell 中快速核对本地文件哈希：

```powershell
Get-FileHash .\Tools3000-v1.5.3-Setup.exe -Algorithm SHA256
```

> 如遇 Windows Defender SmartScreen 未知发布者提示，核对上述 SHA-256 与 VirusTotal 无误后点击「更多信息 -> 仍要运行」即可；安装包与便携包内均随附 SBOM.spdx.json 软件物料清单。
