# 微软 WinGet (Windows Package Manager) 官方入驻指南

> **关于 WinGet**：WinGet 是微软官方为 Windows 10/11 打造的原生命令行包管理器。入驻后，全球用户无需打开浏览器，直接在终端中敲击 `winget install Tools3000` 即可秒级静默安装。

---

## 1. 软件信息定义 (Package Contract)

* **PackageIdentifier**: `Yy1.Tools3000`
* **PackageName**: `Tools3000`
* **Publisher**: `Yy1 (yuan278501381)`
* **InstallerType**: `inno`
* **Moniker**: `tools3000`
* **安装命令**: `winget install Tools3000` 或 `winget install Yy1.Tools3000`

---

## 2. 首次入驻提交流程 (First-time Onboarding)

### 方式一：使用微软官方 `wingetcreate` 工具极速提交 (推荐，最省心)

1. **安装 wingetcreate 工具**（只需执行一次）：
   ```powershell
   winget install Microsoft.WingetCreate
   ```
2. **在 GitHub 创建个人访问令牌 (Personal Access Token, PAT)**：
   * 访问：`https://github.com/settings/tokens`；
   * 生成一个具有 `public_repo` 权限的 Classic Token。
3. **一键生成并自动提 PR**：
   在发布了正式版（如 `v1.0.0`）后，直接运行：
   ```powershell
   wingetcreate new https://github.com/yuan278501381/tools3000/releases/download/v1.0.0/Tools3000-Setup.exe
   ```
   工具会自动下载安装包、解析 Inno Setup 结构、计算 SHA256，并使用您的 GitHub Token 自动在官方 `microsoft/winget-pkgs` 提 PR！

---

### 方式二：使用本项目内置的清单生成工具 (纯手工/精准控制)

1. **一键生成合规清单**：
   ```powershell
   pwsh -File .\scripts\generate-winget-manifest.ps1
   ```
   脚本会自动在 `manifests\winget` 生成以下 4 个标准 YAML，并调用 `winget validate` 确保 0 警告通过：
   * `Yy1.Tools3000.version.yaml`
   * `Yy1.Tools3000.installer.yaml`
   * `Yy1.Tools3000.locale.zh-CN.yaml`
   * `Yy1.Tools3000.locale.en-US.yaml`
2. **提交到官方仓库**：
   * Fork 官方仓库 [microsoft/winget-pkgs](https://github.com/microsoft/winget-pkgs)；
   * 在你的 Fork 仓库中创建目录：
     `manifests/y/Yy1/Tools3000/<版本号>/`
   * 将上述 4 个 YAML 文件拷贝放入该目录并提交 Commit；
   * 向 `microsoft/winget-pkgs:master` 发起 Pull Request；
   * 微软官方自动化机器人（`winget-automation`）会在数分钟内自动跑完沙箱安装/卸载检测并合并！合并后全球可用！

---

## 3. 后续版本更新维护 (Subsequent Updates)

每次在 GitHub 发布新 Release 后，更新只需 10 秒：
```powershell
wingetcreate update Yy1.Tools3000 -u https://github.com/yuan278501381/tools3000/releases/download/v<新版本>/Tools3000-Setup.exe
```
输入 Token 即可全自动提交新版本 PR，微软机器人自动秒级合入！
