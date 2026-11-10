# SignPath.io Foundation 免费开源代码签名接入指南

> **关于 SignPath Foundation**：SignPath Foundation 为符合条件的公开开源项目提供 **100% 免费的受信任云代码签名服务**。证书由正规公信 CA 签发，私钥安全托管在符合 FIPS 140-2 Level 2 / CC EAL 4+ 标准的云端硬件安全模块 (HSM) 中，完全免除个人购买高昂商业证书与物理 USB 加密狗的成本。

---

## 1. 申请资格与前提条件 (Eligibility)

根据 SignPath Foundation 政策，Tools3000 (Tools3000) 100% 符合其赞助标准：
- [x] **开源许可证**：采用 OSI 批准的开源协议（本项目为 **MIT License**，见 [LICENSE](file:///c:/repo/tools3000/LICENSE)）；
- [x] **公开可访问**：代码仓库公开托管于 GitHub (`https://github.com/yuan278501381/tools3000`)；
- [x] **透明构建**：构建流水线完全由 GitHub 托管的官方虚拟机（GitHub-Hosted Runner）公开执行，杜绝私下构建投毒；
- [x] **无恶意软件历史**：项目不包含广告插件、挖矿代码或恶意行为。

---

## 2. 第一步：在线提交 SignPath 免费项目申请

1. **访问官方申请入口**：
   打开浏览器访问：[https://signpath.org/solutions/open-source-community](https://signpath.org/solutions/open-source-community) 或 [https://about.signpath.io/programs/open-source](https://about.signpath.io/programs/open-source)。
2. **填写申请表单 (参考模板)**：
   * **Project Name**: `Tools3000 (Tools3000)`
   * **Project URL / Repository**: `https://github.com/yuan278501381/tools3000`
   * **License**: `MIT License`
   * **Maintainer / Organization**: `Yy1 (yuan278501381)`
   * **Project Description**:
     > Tools3000 is an open-source, lightweight Windows productivity toolkit built with modern C++20 and React. It provides system-wide mouse gestures, screen capture/recording with real-time keycast overlays, and high-performance file indexing services.
   * **Artifacts to be signed**: `Windows Inno Setup Installers (*.exe) and standalone PE binaries (*.exe, *.dll)`
3. **审核周期**：
   SignPath 基金会团队通常在 **3 ~ 7 个工作日** 内完成人工审核，并通过邮件发送批准通知与邀请链接。

---

## 3. 第二步：SignPath 控制台项目与策略配置

收到批准邮件后，登录 SignPath Web 控制台完成以下初始化：

1. **创建项目 (Project)**：
   * 创建名为 `tools3000`（或 `tools3000`）的项目；
   * 记录生成的 **Project Slug**（例如：`tools3000`）。
2. **设置签名策略 (Signing Policy)**：
   * 创建一条 Release 签名策略，Policy Slug 设置为：`release-signing`；
   * 签名证书类型选择：**Open Source Code Signing Certificate**；
   * 签名算法选择：**SHA-256**，启用 **RFC 3161 Timestamping**。
3. **获取 API 凭据**：
   * 在 Organization Settings -> CI/CD Integration 中生成 **API Token**；
   * 记录您的 **Organization ID** 和 **API Token**。

---

## 4. 第三步：在 GitHub 仓库注入安全凭据 (Secrets)

进入 GitHub 官方仓库：
`https://github.com/yuan278501381/tools3000/settings/secrets/actions`

添加以下两个 Actions Repository Secrets：

| Secret 名称 | 说明 | 示例值 |
| :--- | :--- | :--- |
| `SIGNPATH_API_TOKEN` | 在 SignPath 控制台生成的 CI/CD API 访问密钥 | `sp_token_xxxxxxxxxxxx` |
| `SIGNPATH_ORGANIZATION_ID` | 您的 SignPath 组织唯一 UUID | `xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx` |

> 🔒 **安全性保障**：GitHub Secrets 仅在受保护的工作流中作为内存环境变量解密传递，绝不会被输出到构建日志或公开给分支 PR。

---

## 5. 第四步：触发云端全自动签名流水线

当完成上述配置后，仓库内的 `.github/workflows/release.yml` 流水线将全自动接管发版：

1. **本地推送正式版本 Tag**：
   ```bash
   git tag v1.2.0 -m "Release v1.2.0"
   git push origin v1.2.0
   ```
2. **云端全自动流水线流转**：
   - GitHub Actions 启动全新的 `windows-latest` 纯净虚拟机；
   - 自动拉取依赖、编译 C++ 核心与前端、跑通 284+ 项单元测试；
   - 自动生成初始安装包 `Tools3000-Setup.exe`；
   - 调用 `signpath/github-action-submit-signing-request@v2` 将安装包安全提交至 SignPath 云端 HSM；
   - SignPath 在硬件安全模块中为安装包盖上权威 CA 的正规商业数字签名，并打上 DigiCert RFC 3161 时间戳；
   - 签名后的最终安装包回传 GitHub Actions，自动发布至 GitHub Releases 页面供全球用户下载！
