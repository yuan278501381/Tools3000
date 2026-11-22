# Tools3000 v1.3.2 官方正式版发布说明

欢迎使用 **Tools3000 v1.3.2** 官方正式版。以下为相较上一个正式发行版 **v1.3.1** 的窗口渲染修复摘要。

---

## 本次重点修复与体验改进

### 1. 彻底修复无边框窗口最大化闪现 Win98 / 切前台滞留 Win7 旧式标题栏问题
- **Windows 11 原生 DWM 硬件圆角零降级**：重构跨系统通用圆角策略（`WinUtils::applyUniversalRoundedCorners`），在 Windows 11 启用 DWM 合成环境下，对搜索窗口、设置窗口及录屏演播室等标准无边框应用窗口不再叠加 `SetWindowRgn`，完整保留系统原生平滑圆角与高斯阴影，彻底消除最大化瞬间闪现经典 Win98 窗框以及还原后被系统附加 Win7 Basic 顶角裁剪区的问题（感谢 V2EX @lzrainchen）。
- **全链路非客户区重绘防护（全兼容 Win11 / Win10 / Server / RDP）**：统一拦截 `WM_NCACTIVATE` 并完善 `WM_NCCALCSIZE` 全分支处理，在非 DWM 或 Windows 10 / Server RGN 兜底模式下自动接管 `WM_NCPAINT`，同时在 Windows 11 DWM 合成启用态严格保持 `DWMWA_NCRENDERING_ENABLED` 不降级，杜绝窗口最大化、取消最大化与前后台切换时系统 GDI 补绘旧式标题栏（感谢 V2EX @lzrainchen）。
- **消除长时间闲置或资源紧张切回前台时的旧框露底**：为搜索窗口与设置窗口统一补齐与深浅色主题一致的 `WM_PAINT` 实色底衬及不透明 WebView2 默认背景色，并在窗口重新激活瞬间抢跑唤醒已挂起（`TrySuspend`）的渲染器，显著提升后台长驻或高负载下切回前台的首帧观感（感谢 V2EX @lzrainchen）。

---

### 文件完整性校验与安全查验 (SHA-256 / VirusTotal)

| 发布文件 | SHA-256 | 安全检测 |
| :--- | :--- | :--- |
| `Tools3000-v1.3.2-Setup.exe` | `523a6be128a5948a63ff428fb172586d8af597985aa6dc99bd9e2432b576947e` | [VirusTotal 查验](https://www.virustotal.com/gui/file/523a6be128a5948a63ff428fb172586d8af597985aa6dc99bd9e2432b576947e) |
| `Tools3000-v1.3.2-win-x64-portable.zip` | `3870fec61a5249f9a7d8a72cbec06612d1b9a91dad4ad2f8a733f4fab3cc9908` | [VirusTotal 查验](https://www.virustotal.com/gui/file/3870fec61a5249f9a7d8a72cbec06612d1b9a91dad4ad2f8a733f4fab3cc9908) |

下载后可在 PowerShell 中快速核对本地文件哈希：

```powershell
Get-FileHash .\Tools3000-v1.3.2-Setup.exe -Algorithm SHA256
```

> 如遇 Windows Defender SmartScreen 未知发布者提示，核对上述 SHA-256 与 VirusTotal 无误后点击「更多信息 -> 仍要运行」即可；安装包与便携包内均随附 SBOM.spdx.json 软件物料清单。
