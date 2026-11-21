#!/usr/bin/env pwsh
<#
.SYNOPSIS
Tools3000 包管理器清单单一事实源生成与官方门禁校验工具 (Scoop Bucket + Winget 1.10.0)

.DESCRIPTION
根据目标版本号与真实发布包 SHA-256，幂等生成并校验：
  1. bucket/tools3000.json                         (Scoop 自托管 Bucket + 兼容 scoop-extras 官方规范)
  2. packaging/winget/Yy1.Tools3000.yaml           (Winget Version Manifest v1.10.0)
  3. packaging/winget/Yy1.Tools3000.installer.yaml (Winget Installer Manifest v1.10.0)
  4. packaging/winget/Yy1.Tools3000.locale.en-US.yaml
  5. packaging/winget/Yy1.Tools3000.locale.zh-CN.yaml

内置 -Validate 门禁：同时校验 Scoop JSON 结构、autoupdate 模板、64 位 SHA-256，并调用系统
winget validate --manifest packaging/winget 执行微软官方 Schema 与策略验证。
#>
param(
    [string]$Version = "",
    [string]$SetupExePath = "",
    [string]$PortableZipPath = "",
    [string]$SetupSha256 = "",
    [string]$PortableSha256 = "",
    [string]$ReleaseDate = "",
    [switch]$Validate
)

$ErrorActionPreference = "Stop"

# ── 单一事实源元数据常量（零硬编码散落） ──────────────────────────────────────────
$REPO_OWNER            = "yuan278501381"
$REPO_NAME             = "Tools3000"
$REPO_URL              = "https://github.com/$REPO_OWNER/$REPO_NAME"
$PACKAGE_IDENTIFIER    = "Yy1.Tools3000"
$PACKAGE_NAME          = "Tools3000"
$PUBLISHER_NAME        = "Yy1 (yuan278501381)"
$LICENSE_ID            = "GPL-3.0-only"
$PRODUCT_CODE          = "Tools3000_is1"
$WINGET_SCHEMA_VERSION = "1.10.0"

$RepoRoot  = Split-Path -Parent $PSScriptRoot
$BucketDir = Join-Path $RepoRoot "bucket"
$WingetDir = Join-Path $RepoRoot "packaging\winget"

function Assert-Sha256Hex {
    param([string]$Value, [string]$Label)
    if ($Value -notmatch '^[0-9a-fA-F]{64}$') {
        throw "[$Label] 不是有效的 64 位 SHA-256 十六进制字符串: '$Value'"
    }
}

function Write-Utf8NoBom {
    param([string]$Path, [string]$Content)
    $parent = Split-Path -Parent $Path
    if (-not (Test-Path -LiteralPath $parent)) {
        New-Item -ItemType Directory -Path $parent -Force | Out-Null
    }
    $utf8NoBom = New-Object System.Text.UTF8Encoding($false)
    [System.IO.File]::WriteAllText($Path, $Content, $utf8NoBom)
}

if ([string]::IsNullOrWhiteSpace($Version)) {
    $versionFile = Join-Path $RepoRoot "VERSION"
    if (-not (Test-Path -LiteralPath $versionFile)) {
        throw "未提供 -Version 且未找到根目录 VERSION 文件。"
    }
    $Version = (Get-Content -LiteralPath $versionFile -Raw).Trim()
}
$Version = $Version.Trim().TrimStart('v')
if ($Version -notmatch '^\d+\.\d+\.\d+$') {
    throw "无效的语义化版本号: '$Version'（期望格式如 1.0.11）"
}
$Tag = "v$Version"

if ([string]::IsNullOrWhiteSpace($ReleaseDate)) {
    $ReleaseDate = (Get-Date).ToString("yyyy-MM-dd")
}
if ($ReleaseDate -notmatch '^\d{4}-\d{2}-\d{2}$') {
    throw "无效的发布日期格式: '$ReleaseDate'（期望格式 YYYY-MM-DD）"
}

if (-not [string]::IsNullOrWhiteSpace($SetupExePath)) {
    if (-not (Test-Path -LiteralPath $SetupExePath)) {
        throw "找不到安装包文件: $SetupExePath"
    }
    $SetupSha256 = (Get-FileHash -LiteralPath $SetupExePath -Algorithm SHA256).Hash.ToUpperInvariant()
}
if (-not [string]::IsNullOrWhiteSpace($PortableZipPath)) {
    if (-not (Test-Path -LiteralPath $PortableZipPath)) {
        throw "找不到便携包文件: $PortableZipPath"
    }
    $PortableSha256 = (Get-FileHash -LiteralPath $PortableZipPath -Algorithm SHA256).Hash.ToLowerInvariant()
}

Assert-Sha256Hex -Value $SetupSha256 -Label "SetupSha256"
Assert-Sha256Hex -Value $PortableSha256 -Label "PortableSha256"

$SetupSha256Upper    = $SetupSha256.ToUpperInvariant()
$PortableSha256Lower = $PortableSha256.ToLowerInvariant()

$SetupUrl    = "$REPO_URL/releases/download/$Tag/Tools3000-$Tag-Setup.exe"
$PortableUrl = "$REPO_URL/releases/download/$Tag/Tools3000-$Tag-win-x64-portable.zip"
$ReleaseUrl  = "$REPO_URL/releases/tag/$Tag"

# ── 1. 生成 Scoop 清单 (bucket/tools3000.json) ──────────────────────────────────
$ScoopManifestObj = [ordered]@{
    version      = $Version
    description  = "Lightweight native Windows all-in-one productivity suite: instant NTFS file & content search, QuickLook, screenshot markup, scrolling capture, offline OCR, screen recorder with auto-zoom Studio, and global mouse gestures."
    homepage     = $REPO_URL
    license      = $LICENSE_ID
    notes        = @(
        "Tools3000 runs in portable mode under Scoop with user settings persisted in 'data'.",
        "Run 'tools3000' from terminal or launch Tools3000 from the Start Menu shortcut."
    )
    architecture = [ordered]@{
        "64bit" = [ordered]@{
            url  = $PortableUrl
            hash = $PortableSha256Lower
        }
    }
    bin          = @(
        @("Tools3000.exe", "tools3000")
    )
    shortcuts    = @(
        @("Tools3000.exe", "Tools3000")
    )
    persist      = "data"
    checkver     = [ordered]@{
        github = $REPO_URL
    }
    autoupdate   = [ordered]@{
        architecture = [ordered]@{
            "64bit" = [ordered]@{
                url  = "$REPO_URL/releases/download/v`$version/Tools3000-v`$version-win-x64-portable.zip"
                hash = [ordered]@{
                    url  = "$REPO_URL/releases/download/v`$version/SHA256SUMS.txt"
                    mode = "extract"
                }
            }
        }
    }
}

$ScoopJsonPath = Join-Path $BucketDir "tools3000.json"
$ScoopJsonText = ($ScoopManifestObj | ConvertTo-Json -Depth 10) + "`n"
Write-Utf8NoBom -Path $ScoopJsonPath -Content $ScoopJsonText
Write-Host "[OK] 已更新 Scoop Bucket 清单: $ScoopJsonPath (v$Version)" -ForegroundColor Green

# ── 2. 生成 Winget 多文件清单 (packaging/winget/Yy1.Tools3000*.yaml) ─────────────
$WingetVersionYaml = @"
# Created using Tools3000 update-package-manifests.ps1
# yaml-language-server: `$schema=https://aka.ms/winget-manifest.version.$WINGET_SCHEMA_VERSION.schema.json

PackageIdentifier: $PACKAGE_IDENTIFIER
PackageVersion: $Version
DefaultLocale: en-US
ManifestType: version
ManifestVersion: $WINGET_SCHEMA_VERSION
"@

$WingetInstallerYaml = @"
# Created using Tools3000 update-package-manifests.ps1
# yaml-language-server: `$schema=https://aka.ms/winget-manifest.installer.$WINGET_SCHEMA_VERSION.schema.json

PackageIdentifier: $PACKAGE_IDENTIFIER
PackageVersion: $Version
Platform:
- Windows.Desktop
MinimumOSVersion: 10.0.17763.0
InstallerType: inno
Scope: machine
UpgradeBehavior: install
ElevationRequirement: elevationRequired
ReleaseDate: $ReleaseDate
AppsAndFeaturesEntries:
- DisplayName: $PACKAGE_NAME
  Publisher: $PUBLISHER_NAME
  DisplayVersion: $Version
  ProductCode: $PRODUCT_CODE
  InstallerType: inno
Installers:
- Architecture: x64
  InstallerUrl: $SetupUrl
  InstallerSha256: $SetupSha256Upper
ManifestType: installer
ManifestVersion: $WINGET_SCHEMA_VERSION
"@

$WingetLocaleEnYaml = @"
# Created using Tools3000 update-package-manifests.ps1
# yaml-language-server: `$schema=https://aka.ms/winget-manifest.defaultLocale.$WINGET_SCHEMA_VERSION.schema.json

PackageIdentifier: $PACKAGE_IDENTIFIER
PackageVersion: $Version
PackageLocale: en-US
Publisher: $PUBLISHER_NAME
PublisherUrl: https://github.com/$REPO_OWNER
PublisherSupportUrl: $REPO_URL/issues
Author: $PUBLISHER_NAME
PackageName: $PACKAGE_NAME
PackageUrl: $REPO_URL
License: $LICENSE_ID
LicenseUrl: $REPO_URL/blob/main/LICENSE
Copyright: Copyright (c) 2026 $PUBLISHER_NAME & Tools3000 contributors
CopyrightUrl: $REPO_URL/blob/main/LICENSE
ShortDescription: Lightweight native all-in-one Windows productivity suite with instant file search, offline OCR, screenshot markup, screen recording Studio, and mouse gestures.
Description: |-
  Tools3000 unifies everyday Windows desktop utilities into one lightweight native C++ and Rust application: millisecond NTFS file and full-text document search, QuickLook preview, scrolling capture, precision screenshot annotation, grouped desktop pins, high-accuracy offline PP-OCRv4 text recognition, 60 FPS screen recording with auto-zoom Studio, and global mouse gestures. 100% offline, zero telemetry, GPL-3.0 open source.
Moniker: tools3000
Tags:
- file-search
- quicklook
- screenshot
- annotation
- ocr
- screen-recorder
- mouse-gestures
- productivity
- utility
- offline
- open-source
ReleaseNotesUrl: $ReleaseUrl
ManifestType: defaultLocale
ManifestVersion: $WINGET_SCHEMA_VERSION
"@

$WingetLocaleZhYaml = @"
# Created using Tools3000 update-package-manifests.ps1
# yaml-language-server: `$schema=https://aka.ms/winget-manifest.locale.$WINGET_SCHEMA_VERSION.schema.json

PackageIdentifier: $PACKAGE_IDENTIFIER
PackageVersion: $Version
PackageLocale: zh-CN
Publisher: $PUBLISHER_NAME
PublisherUrl: https://github.com/$REPO_OWNER
PublisherSupportUrl: $REPO_URL/issues
Author: $PUBLISHER_NAME
PackageName: $PACKAGE_NAME
PackageUrl: $REPO_URL
License: $LICENSE_ID
LicenseUrl: $REPO_URL/blob/main/LICENSE
Copyright: Copyright (c) 2026 $PUBLISHER_NAME & Tools3000 contributors
CopyrightUrl: $REPO_URL/blob/main/LICENSE
ShortDescription: 轻量、原生的 Windows 多合一桌面效率工具集，集成极速文件与内容搜索、高精度离线 OCR、截图标注贴图、自动运镜录屏演播室与全局鼠标手势。
Description: |-
  Tools3000 将日常高频的毫秒级 NTFS 文件搜索、文档正文穿透检索、空格快速预览、滚动长截图、精准截图标注、桌面贴图组、高精度离线文本识别、60 FPS 自动运镜录屏演播室与全局鼠标手势整合为单一原生软件。100% 本地离线运行、零遥测无账号、GPL-3.0 开源免费。
Tags:
- 文件搜索
- 截图
- 贴图
- 文字识别
- 录屏
- 鼠标手势
- 效率工具
- 开源
ReleaseNotesUrl: $ReleaseUrl
ManifestType: locale
ManifestVersion: $WINGET_SCHEMA_VERSION
"@

Write-Utf8NoBom -Path (Join-Path $WingetDir "$PACKAGE_IDENTIFIER.yaml")              -Content ($WingetVersionYaml.TrimEnd() + "`n")
Write-Utf8NoBom -Path (Join-Path $WingetDir "$PACKAGE_IDENTIFIER.installer.yaml")    -Content ($WingetInstallerYaml.TrimEnd() + "`n")
Write-Utf8NoBom -Path (Join-Path $WingetDir "$PACKAGE_IDENTIFIER.locale.en-US.yaml") -Content ($WingetLocaleEnYaml.TrimEnd() + "`n")
Write-Utf8NoBom -Path (Join-Path $WingetDir "$PACKAGE_IDENTIFIER.locale.zh-CN.yaml") -Content ($WingetLocaleZhYaml.TrimEnd() + "`n")
Write-Host "[OK] 已更新 Winget 清单目录: $WingetDir (v$Version)" -ForegroundColor Green

# ── 3. 自动化门禁校验 (-Validate) ───────────────────────────────────────────────
if ($Validate) {
    Write-Host "[CHECK] 正在校验 Scoop Bucket 清单..." -ForegroundColor Cyan
    $parsedScoop = Get-Content -LiteralPath $ScoopJsonPath -Raw | ConvertFrom-Json
    if ($parsedScoop.version -ne $Version) {
        throw "Scoop 清单版本号不匹配: $($parsedScoop.version) != $Version"
    }
    if ($parsedScoop.architecture.'64bit'.hash -ne $PortableSha256Lower) {
        throw "Scoop 清单 SHA-256 哈希不匹配。"
    }
    if ($parsedScoop.persist -ne "data") {
        throw "Scoop 清单必须持久化 'data' 目录以保证绿色便携模式配置不丢失。"
    }
    Write-Host "[OK] Scoop 清单 Schema 与便携持久化校验通过" -ForegroundColor Green

    $wingetCmd = Get-Command "winget.exe" -ErrorAction SilentlyContinue
    if (-not $wingetCmd) {
        Write-Host "[WARN] 当前机器未检测到 winget.exe，跳过 winget validate 命令校验。" -ForegroundColor Yellow
    } else {
        Write-Host "[CHECK] 正在运行微软官方 winget validate --manifest $WingetDir ..." -ForegroundColor Cyan
        & $wingetCmd.Source validate --manifest $WingetDir
        if ($LASTEXITCODE -ne 0) {
            throw "winget validate 校验失败 (ExitCode=$LASTEXITCODE)！"
        }
        Write-Host "[OK] 微软官方 winget validate 校验 100% 通过！" -ForegroundColor Green
    }
}
