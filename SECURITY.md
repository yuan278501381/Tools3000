# 安全策略与可信验证说明 (Security Policy)

感谢您关注 **Tools3000** 的安全性与透明度。Tools3000 是一款完全离线运行、零遥测、无账号体系的 Windows 原生开源软件（MIT 协议）。

---

## 1. 四重零黑盒验证方式

为消除用户对 Windows 桌面工具的安全顾虑，每次正式发布均提供完整的公开审计链：

1. **SHA-256 完整性校验**：每个 [GitHub Release](https://github.com/yuan278501381/Tools3000/releases) 均附带 `SHA256SUMS.txt`。下载后可在 PowerShell 中核对：
   ```powershell
   Get-FileHash .\Tools3000-Setup.exe -Algorithm SHA256
   ```
2. **VirusTotal 公开多引擎查验**：在 [VirusTotal](https://www.virustotal.com/) 搜索栏直接输入发布包的 SHA-256（或点击每次 Release 说明中的直达链接），即可核验 70+ 款主流安全引擎的公开检测结果。
3. **内置 SPDX 软件物料清单（SBOM）**：安装包与便携包根目录均随附 `SBOM.spdx.json` 与 `THIRD_PARTY_NOTICES.txt`，完整列出全部开源第三方依赖与许可证信息。
4. **完全开源自编译（零黑盒）**：所有 C++ / Rust / React 源码全部公开，任何用户均可通过 [开发者指南](docs/DEVELOPER.md) 在本机从源码独立编译验证。

---

## 2. 关于 Windows Defender SmartScreen 提示

在独立开源项目获取商业 EV / 公网可信代码签名（如 SignPath OSS 或 Azure Trusted Signing）或新版本完成云端声誉累积之前，Windows Defender SmartScreen 可能会对刚下载的安装包显示「Windows 已保护你的电脑 / 未知发布者」提示。这是 Windows 对新发布无商业证书二进制文件的常规信誉保护机制，并不代表程序包含恶意行为。

在核对 SHA-256 哈希与 VirusTotal 检测结果无误后，可点击「更多信息 -> 仍要运行」继续安装，或使用便携版 / 源码自编译版本。

---

## 3. 报告安全漏洞

如果您发现潜在的安全隐患（如权限边界越权、进程间通信注入、剪贴板/OCR 敏感数据泄露风险或依赖库高危 CVE），**请勿直接在公开 Issue 中披露利用细节**，建议通过以下私密渠道联系维护者：

- **首选方式**：使用本仓库的 **[GitHub Private Vulnerability Reporting (Security Advisories)](https://github.com/yuan278501381/Tools3000/security/advisories/new)** 私密提交；
- 请尽量附带复现步骤、受影响版本、Windows 版本与系统架构（`x64` / `ARM64`）。

维护者将在收到报告后的 **72 小时内** 响应确认，并与您协同推进修复与披露节奏。

---

## 4. 支持的安全更新版本

| 版本 | 安全维护状态 |
| :--- | :--- |
| 最新正式版 (`Latest Release`) | 持续提供安全修复与快速响应 |
| 历史旧版本 | 请升级至最新版本 |
