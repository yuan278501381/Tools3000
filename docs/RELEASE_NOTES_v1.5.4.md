# Tools3000 v1.5.4 Release Notes | 官方正式版发布说明

**[English](#english-release-highlights)** · **[简体中文说明](#本次重点更新与体验改进)** · **[Upgrade / 升级方式](#升级方式)**

---

## English Release Highlights

Welcome to **Tools3000 v1.5.4** (compared against **v1.5.3**). This release focuses on polishing the **Mouse Spotlight** experience based on real-world community feedback, making shake-to-find rock-solid against accidental triggers and ensuring the dismiss expansion animation runs at full native refresh rate with zero frame drops:

1. **Zero-Stutter Spotlight Dismiss Expansion Animation (`#33`, thanks to `@LiangDongXing-cn`)**
   - Completely eliminated frame drops, 30 FPS half-rate lockups, and abrupt final-frame flashes when dismissing the Mouse Spotlight (whether triggered by shaking the mouse or double-tapping `Ctrl`).
   - Rebuilt the fullscreen spotlight mask, expanding ripple rings, and VSync frame pacing so dismiss animations stay silky-smooth at your monitor's full native refresh rate — even on 4K high-DPI displays and with large spotlight sizes.
2. **Anti-False-Trigger Mouse Shake Detection & Configurable Minimum Shake Distance (`#33`, thanks to `@LiangDongXing-cn`)**
   - Redesigned the shake-to-find cursor detector to eliminate accidental triggers during everyday fast diagonal cursor sweeps, tiny hand jitters, slow browsing, and click-and-drag selections or mouse gestures.
   - Added a new **Minimum Move Distance** setting under `Settings -> Mouse Spotlight -> Shake Mouse` (`100–2000 px`, default `400 px`, automatically DPI-normalized across displays) so you can fine-tune exactly how vigorous a shake is required to summon the spotlight.

**Upgrade**: In-app via `Settings -> Insights & About -> About -> Check for Updates` (supports both installer and portable builds), overwrite with `Tools3000-v1.5.4-Setup.exe` / `Tools3000-v1.5.4-win-x64-portable.zip`, or run `scoop update tools3000`.

---

## 本次重点更新与体验改进

欢迎升级至 **Tools3000 v1.5.4**。本版本以已正式发布的 **v1.5.3** 为对比基线，围绕社区用户的实机视频与使用反馈，专项打磨了**鼠标聚光灯**的退场流畅度与摇晃唤醒抗误触体验：

### 1. 彻底解决取消聚光灯时退场放大特效掉帧问题（感谢 GitHub `#33` `@LiangDongXing-cn`）
- 彻底修复晃动鼠标或双击 `Ctrl` 唤出聚光灯后，取消退出时圆圈扩张放大特效偶发掉帧、帧率减半或末尾衔接不够柔顺的问题；
- 优化全屏聚光灯暗幕、扩散水波光环与垂直同步节拍，无论常规尺寸还是 4K 高分屏、大尺寸聚光灯下，退场放大过程全程保持屏幕原生高刷满帧丝滑过渡。

### 2. 重构「摇晃鼠标寻光标」抗误触体验并新增最小移动距离设置（感谢 GitHub `#33` `@LiangDongXing-cn`）
- 彻底解决此前日常快速斜向移动鼠标或轻微手抖时，即使将摇晃阈值调高到 `10` 次仍容易误弹出聚焦圈的痛点；
- 在 `设置 -> 鼠标聚光灯 -> 摇晃鼠标` 中新增 **最小移动距离** 配置项（默认 `400 px`，支持 `100 ~ 2000 px` 自由调节，自动适配多屏 DPI 缩放），仅在快速、大幅度主动来回晃动鼠标时精准触发；
- 日常慢速移动、细微抖动以及按住鼠标按键框选、拖拽窗口或划动鼠标手势期间自动静默，告别误触打扰。

---

## 升级方式

- **应用内一键升级（安装版与绿色便携版均支持）**：打开 `设置 -> 洞察与关于 -> 关于 -> 检查更新`（或点击系统托盘更新提示条）一键下载并自动完成原地升级，完整保留全部配置、剪贴板历史、固定选区与离线模型；
- **标准安装版手动覆盖**：下载下方 `Tools3000-v1.5.4-Setup.exe` 直接运行覆盖安装；
- **绿色便携版手动覆盖**：下载 `Tools3000-v1.5.4-win-x64-portable.zip` 解压覆盖程序目录（保留 `data/` 目录配置与数据）；
- **Scoop 用户**：在终端执行 `scoop update tools3000`。

---

### 文件完整性校验与安全查验 (SHA-256 / VirusTotal)

| 发布文件 | SHA-256 | 安全检测 |
| :--- | :--- | :--- |
| `Tools3000-v1.5.4-Setup.exe` | `51f54ab33fb75ff97722f4b83c7c4ae4c6a35f943c869f91c1dfca87de109035` | [VirusTotal 查验](https://www.virustotal.com/gui/file/51f54ab33fb75ff97722f4b83c7c4ae4c6a35f943c869f91c1dfca87de109035) |
| `Tools3000-v1.5.4-win-x64-portable.zip` | `cffcbcfa286df5df2d4afa7d7c0ae0be5600abfd06796dcbbf6684d6ed44483e` | [VirusTotal 查验](https://www.virustotal.com/gui/file/cffcbcfa286df5df2d4afa7d7c0ae0be5600abfd06796dcbbf6684d6ed44483e) |

下载后可在 PowerShell 中快速核对本地文件哈希：

```powershell
Get-FileHash .\Tools3000-v1.5.4-Setup.exe -Algorithm SHA256
```

> 如遇 Windows Defender SmartScreen 未知发布者提示，核对上述 SHA-256 与 VirusTotal 无误后点击「更多信息 -> 仍要运行」即可；安装包与便携包内均随附 SBOM.spdx.json 软件物料清单。
