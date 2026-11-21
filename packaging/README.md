# Tools3000 包管理器分发与官方源提交流程

本目录与仓库根目录的 [`bucket/tools3000.json`](../bucket/tools3000.json) 均由 [`scripts/update-package-manifests.ps1`](../scripts/update-package-manifests.ps1) 作为单一事实源自动生成，并已接入 [`scripts/release.ps1`](../scripts/release.ps1) 发版流水线。

---

## 1. Scoop（已即时可用 + 可提交至 `ScoopInstaller/Extras`）

仓库根目录包含标准 `bucket/tools3000.json`，用户无需等待任何审核即可直接添加本仓库作为自托管 Bucket 安装，且每次发版后自动保持同步：

```powershell
scoop bucket add tools3000 https://github.com/yuan278501381/Tools3000
scoop install tools3000/tools3000
```

- Scoop 会自动创建并持久化 `data/` 目录，天然激活 Tools3000 的绿色便携模式，版本升级时用户配置、热键、手势与数据零丢失。
- **提交至官方 `ScoopInstaller/Extras`**：将 `bucket/tools3000.json` 复制到 fork 后的 `ScoopInstaller/Extras` 仓库的 `bucket/tools3000.json` 并提交 Pull Request（清单已包含 `checkver` 与 `autoupdate`，合入后官方机器人将自动追踪后续 Release）。

---

## 2. Winget（符合 `microsoft/winget-pkgs` 1.10.0 官方规范）

目录 [`packaging/winget/`](winget/) 包含 4 份完整的微软官方多文件清单：

- `Yy1.Tools3000.yaml`
- `Yy1.Tools3000.installer.yaml`
- `Yy1.Tools3000.locale.en-US.yaml`
- `Yy1.Tools3000.locale.zh-CN.yaml`

### 本地验证（微软官方验证器）

```powershell
winget validate --manifest packaging/winget
```

### 提交至微软官方 `microsoft/winget-pkgs`

方式 A（使用微软官方 `wingetcreate` 一键提 PR，推荐）：

```powershell
winget install Microsoft.WingetCreate
wingetcreate submit packaging/winget
```

方式 B（直接向 `microsoft/winget-pkgs` 提交 PR）：
将 `packaging/winget/` 中 4 个文件放入 `manifests/y/Yy1/Tools3000/<Version>/` 目录并提 PR；合入后全球 Windows 用户即可直接使用：

```powershell
winget install Yy1.Tools3000
```

---

## 3. 手动刷新清单

发版流水线会自动执行刷新；如需针对指定版本手工重算：

```powershell
pwsh -File scripts/update-package-manifests.ps1 `
  -Version 1.0.11 `
  -SetupExePath Output\Tools3000-Setup.exe `
  -PortableZipPath Output\Tools3000-win-x64-portable.zip `
  -Validate
```
