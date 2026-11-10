<#
.SYNOPSIS
Tools3000 微软 WinGet (winget-pkgs) 官方清单一键生成与校验工具

.DESCRIPTION
根据当前版本号 (VERSION) 与本地或 GitHub Release 产物计算 SHA-256 哈希，
全自动生成符合微软 WinGet v1.9.0 最新标准的三件套清单 (version, installer, defaultLocale, locale)，
并支持使用 winget validate 进行本地语法强校验。

.EXAMPLE
.\scripts\generate-winget-manifest.ps1
.\scripts\generate-winget-manifest.ps1 -InstallerPath "Output\Tools3000-Setup.exe" -Validate
#>

param(
    [string]$InstallerPath = "Output\Tools3000-Setup.exe",
    [string]$OutputDir = "manifests\winget",
    [switch]$Validate = $true
)

$ErrorActionPreference = "Stop"
$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Definition
$ProjectRoot = Split-Path -Parent $ScriptDir
Set-Location $ProjectRoot

$VersionFile = Join-Path $ProjectRoot "VERSION"
if (-not (Test-Path $VersionFile)) {
    throw "VERSION 文件丢失: $VersionFile"
}
$Version = (Get-Content -LiteralPath $VersionFile -Raw).Trim()

if (-not (Test-Path $InstallerPath)) {
    throw "安装包未找到: $InstallerPath；请先执行 .\deploy.ps1 生成安装包。"
}

Write-Host "=======================================================" -ForegroundColor Cyan
Write-Host "Tools3000 WinGet 官方清单自动化生成工具" -ForegroundColor Cyan
Write-Host "目标版本: $Version" -ForegroundColor Green
Write-Host "安装包文件: $InstallerPath" -ForegroundColor Green
Write-Host "=======================================================" -ForegroundColor Cyan

$Sha256 = (Get-FileHash -Path $InstallerPath -Algorithm SHA256).Hash.ToUpperInvariant()
Write-Host "安装包 SHA-256 哈希: $Sha256" -ForegroundColor Yellow

if (-not (Test-Path $OutputDir)) {
    New-Item -ItemType Directory -Path $OutputDir -Force | Out-Null
}

$ReleaseDate = (Get-Date).ToString("yyyy-MM-dd")
$DownloadUrl = "https://github.com/yuan278501381/tools3000/releases/download/v$Version/Tools3000-Setup.exe"

# 1. version.yaml
$VersionYaml = @"
# yaml-language-server: `$schema=https://aka.ms/winget-manifest.version.1.9.0.schema.json

PackageIdentifier: Yy1.Tools3000
PackageVersion: $Version
DefaultLocale: zh-CN
ManifestType: version
ManifestVersion: 1.9.0
"@

# 2. installer.yaml
$InstallerYaml = @"
# yaml-language-server: `$schema=https://aka.ms/winget-manifest.installer.1.9.0.schema.json

PackageIdentifier: Yy1.Tools3000
PackageVersion: $Version
InstallerLocale: zh-CN
Platform:
  - Windows.Desktop
MinimumOSVersion: 10.0.17763.0
InstallerType: inno
Scope: machine
InstallModes:
  - interactive
  - silent
  - silentWithProgress
InstallerSwitches:
  Silent: /VERYSILENT /NORESTART /ALLUSERS
  SilentWithProgress: /SILENT /NORESTART /ALLUSERS
UpgradeBehavior: install
ReleaseDate: $ReleaseDate
Installers:
  - Architecture: x64
    InstallerUrl: $DownloadUrl
    InstallerSha256: $Sha256
    ProductCode: Tools3000_is1
ManifestType: installer
ManifestVersion: 1.9.0
"@

# 3. locale.zh-CN.yaml
$ZhLocaleYaml = @"
# yaml-language-server: `$schema=https://aka.ms/winget-manifest.defaultLocale.1.9.0.schema.json

PackageIdentifier: Yy1.Tools3000
PackageVersion: $Version
PackageLocale: zh-CN
Publisher: Yy1 (yuan278501381)
PublisherUrl: https://github.com/yuan278501381
PublisherSupportUrl: https://github.com/yuan278501381/tools3000/issues
Author: Yy1 (yuan278501381) & Tools3000 contributors
PackageName: Tools3000
PackageUrl: https://github.com/yuan278501381/tools3000
License: MIT
LicenseUrl: https://github.com/yuan278501381/tools3000/blob/main/LICENSE
Copyright: Copyright (c) 2026 Yy1 (yuan278501381) & Tools3000 contributors
CopyrightUrl: https://github.com/yuan278501381/tools3000/blob/main/LICENSE
ShortDescription: 极致轻量、现代化的 Windows 桌面效率工具箱，支持全局鼠标手势、高清录屏与按键回显、秒级文件索引搜索。
Description: |
  Tools3000 (Tools3000) 是一款基于现代 C++20 与 React 深度融合打造的高性能 Windows 桌面级效率工具箱：
  - 毫秒级全局鼠标手势：智能识别连贯笔画与拐角，提供饱满纯净的微晶高光反馈，穿透灵敏不卡手；
  - 屏幕录制与按键回显：高帧率无损选区录屏，实时次像素抗锯齿 Keycast 按键浮层联动；
  - 秒级文件极速检索：按需唤醒常驻的 Windows 服务，支持 NTFS USN 日志秒级扫描全盘；
  - 极限轻量与工作集修剪：冷路径物理内存归还，无冗余常驻开销。
Moniker: tools3000
Tags:
  - productivity
  - gesture
  - mouse-gesture
  - screen-recorder
  - keycast
  - search
  - efficiency
  - tools
ReleaseNotesUrl: https://github.com/yuan278501381/tools3000/releases/tag/v$Version
ManifestType: defaultLocale
ManifestVersion: 1.9.0
"@

# 4. locale.en-US.yaml
$EnLocaleYaml = @"
# yaml-language-server: `$schema=https://aka.ms/winget-manifest.locale.1.9.0.schema.json

PackageIdentifier: Yy1.Tools3000
PackageVersion: $Version
PackageLocale: en-US
Publisher: Yy1 (yuan278501381)
PublisherUrl: https://github.com/yuan278501381
PublisherSupportUrl: https://github.com/yuan278501381/tools3000/issues
Author: Yy1 (yuan278501381) & Tools3000 contributors
PackageName: Tools3000
PackageUrl: https://github.com/yuan278501381/tools3000
License: MIT
LicenseUrl: https://github.com/yuan278501381/tools3000/blob/main/LICENSE
Copyright: Copyright (c) 2026 Yy1 (yuan278501381) & Tools3000 contributors
CopyrightUrl: https://github.com/yuan278501381/tools3000/blob/main/LICENSE
ShortDescription: Ultra-lightweight and modern Windows productivity toolkit with global mouse gestures, screen recording, keycast overlays, and instant search.
Description: |
  Tools3000 (Tools3000) is a high-performance Windows desktop productivity toolkit engineered with modern C++20 and React:
  - Global Mouse Gestures: Sub-millisecond stroke recognition with pure microcrystalline glow feedback;
  - Screen Capture & Keycast Overlay: Lossless high-framerate area recording linked with real-time keystroke HUD;
  - Instant File Search: Demand-start background Windows Service with sub-second NTFS USN journal indexing;
  - Extreme Lightweight: Working set memory trimming on cold paths with zero redundant resource consumption.
Tags:
  - productivity
  - gesture
  - mouse-gesture
  - screen-recorder
  - keycast
  - search
  - efficiency
  - tools
ReleaseNotesUrl: https://github.com/yuan278501381/tools3000/releases/tag/v$Version
ManifestType: locale
ManifestVersion: 1.9.0
"@

[System.IO.File]::WriteAllText((Join-Path $OutputDir "Yy1.Tools3000.version.yaml"), $VersionYaml, [System.Text.Encoding]::UTF8)
[System.IO.File]::WriteAllText((Join-Path $OutputDir "Yy1.Tools3000.installer.yaml"), $InstallerYaml, [System.Text.Encoding]::UTF8)
[System.IO.File]::WriteAllText((Join-Path $OutputDir "Yy1.Tools3000.locale.zh-CN.yaml"), $ZhLocaleYaml, [System.Text.Encoding]::UTF8)
[System.IO.File]::WriteAllText((Join-Path $OutputDir "Yy1.Tools3000.locale.en-US.yaml"), $EnLocaleYaml, [System.Text.Encoding]::UTF8)

Write-Host "✅ 成功生成全套 WinGet 1.9.0 清单至: $OutputDir" -ForegroundColor Green

if ($Validate) {
    if (Get-Command "winget" -ErrorAction SilentlyContinue) {
        Write-Host "正在调用系统 winget 验证清单合法性..." -ForegroundColor Cyan
        & winget validate --manifest $OutputDir
        if ($LASTEXITCODE -eq 0) {
            Write-Host "🎉 微软 WinGet 规范校验 100% 通过！" -ForegroundColor Green
        } else {
            Write-Warning "WinGet 验证返回退出码: $LASTEXITCODE"
        }
    } else {
        Write-Host "未检测到系统 winget 命令，跳过本地验证。" -ForegroundColor Yellow
    }
}
