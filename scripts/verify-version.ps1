#!/usr/bin/env pwsh
param(
    [string]$Tag = "",
    [string]$BinaryDir = "",
    [switch]$RequireBinary
)

$ErrorActionPreference = "Stop"
$ProjectRoot = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
$VersionFile = Join-Path $ProjectRoot "VERSION"

if (-not (Test-Path -LiteralPath $VersionFile)) {
    throw "缺少产品版本单一事实源: $VersionFile"
}

$Version = (Get-Content -LiteralPath $VersionFile -Raw).Trim()
if ($Version -notmatch '^(\d+)\.(\d+)\.(\d+)$') {
    throw "VERSION 必须包含唯一稳定 SemVer 版本号（例如 1.2.3），当前值: $Version"
}
$Major = [int]$Matches[1]
$Minor = [int]$Matches[2]
$Patch = [int]$Matches[3]
$ExpectedQuadComma = "$Major,$Minor,$Patch,0"
$ExpectedQuadDot = "$Version.0"

# 1. 校验前端 package.json
$UiPackageJson = Join-Path $ProjectRoot "ui\package.json"
if (Test-Path -LiteralPath $UiPackageJson) {
    $Pkg = Get-Content -LiteralPath $UiPackageJson -Raw | ConvertFrom-Json
    if ([string]$Pkg.version -ne $Version) {
        throw "ui/package.json 版本号 ($($Pkg.version)) 与根目录 VERSION ($Version) 不一致"
    }
}

# 2. 校验 Rust Picasa 看图器 Cargo.toml
$ViewerCargoToml = Join-Path $ProjectRoot "src\viewer\Cargo.toml"
if (-not (Test-Path -LiteralPath $ViewerCargoToml)) {
    throw "缺少 Rust 看图器清单文件: $ViewerCargoToml"
}
$CargoRaw = [System.IO.File]::ReadAllText($ViewerCargoToml, [System.Text.Encoding]::UTF8)
$CargoMatch = [regex]::Match($CargoRaw, '(?ms)^\[package\][^\[]*?^version\s*=\s*"([^"]+)"')
if (-not $CargoMatch.Success) {
    throw "无法从 src/viewer/Cargo.toml 读取 [package] version 字段"
}
if ($CargoMatch.Groups[1].Value -ne $Version) {
    throw "src/viewer/Cargo.toml 版本号 ($($CargoMatch.Groups[1].Value)) 与根目录 VERSION ($Version) 不一致"
}

# 3. 校验 Rust Picasa 看图器 Windows 资源文件 viewer.rc
$ViewerRc = Join-Path $ProjectRoot "src\viewer\viewer.rc"
if (-not (Test-Path -LiteralPath $ViewerRc)) {
    throw "缺少 Rust 看图器资源文件: $ViewerRc"
}
$RcRaw = [System.IO.File]::ReadAllText($ViewerRc, [System.Text.Encoding]::UTF8)
$RcChecks = @(
    @{ Name = "FILEVERSION";    Pattern = '(?m)^\s*FILEVERSION\s+(\d+,\d+,\d+,\d+)';  Expected = $ExpectedQuadComma },
    @{ Name = "PRODUCTVERSION"; Pattern = '(?m)^\s*PRODUCTVERSION\s+(\d+,\d+,\d+,\d+)'; Expected = $ExpectedQuadComma },
    @{ Name = "FileVersion";    Pattern = 'VALUE\s+"FileVersion"\s*,\s*"([^"]+)"';    Expected = $ExpectedQuadDot },
    @{ Name = "ProductVersion"; Pattern = 'VALUE\s+"ProductVersion"\s*,\s*"([^"]+)"'; Expected = $Version }
)
foreach ($Chk in $RcChecks) {
    $m = [regex]::Match($RcRaw, $Chk.Pattern)
    if (-not $m.Success) {
        throw "无法从 src/viewer/viewer.rc 解析 $($Chk.Name) 字段"
    }
    if ($m.Groups[1].Value -ne $Chk.Expected) {
        throw "src/viewer/viewer.rc $($Chk.Name) ($($m.Groups[1].Value)) 与根目录 VERSION ($($Chk.Expected)) 不一致"
    }
}

# 4. 校验 Rust Picasa 看图器清单文件 viewer.manifest
$ViewerManifest = Join-Path $ProjectRoot "src\viewer\viewer.manifest"
if (-not (Test-Path -LiteralPath $ViewerManifest)) {
    throw "缺少 Rust 看图器清单文件: $ViewerManifest"
}
$ManifestRaw = [System.IO.File]::ReadAllText($ViewerManifest, [System.Text.Encoding]::UTF8)
$ManifestMatch = [regex]::Match($ManifestRaw, '<assemblyIdentity\s+version="([^"]+)"[^>]*name="Yy1\.Tools3000\.PicasaViewer"')
if (-not $ManifestMatch.Success) {
    throw "无法从 src/viewer/viewer.manifest 解析 Yy1.Tools3000.PicasaViewer assemblyIdentity version"
}
if ($ManifestMatch.Groups[1].Value -ne $ExpectedQuadDot) {
    throw "src/viewer/viewer.manifest assemblyIdentity 版本 ($($ManifestMatch.Groups[1].Value)) 与根目录 VERSION ($ExpectedQuadDot) 不一致"
}

# 5. 校验 Tag 参数（如传入）
if (-not [string]::IsNullOrWhiteSpace($Tag)) {
    if ($Tag -notmatch '^v\d+\.\d+\.\d+$') {
        throw "发布标签必须采用 vMAJOR.MINOR.PATCH 格式: $Tag"
    }
    if ($Tag.Substring(1) -ne $Version) {
        throw "发布标签 $Tag 与根目录 VERSION ($Version) 不一致"
    }
}

# 6. 校验已构建二进制 Tools3000_Picasa_Viewer.exe 的 ProductVersion
$ResolvedBinaryDir = if (-not [string]::IsNullOrWhiteSpace($BinaryDir)) {
    $BinaryDir
} else {
    Join-Path $ProjectRoot "deploy_dist"
}
$ViewerExePath = Join-Path $ResolvedBinaryDir "Tools3000_Picasa_Viewer.exe"
if (Test-Path -LiteralPath $ViewerExePath) {
    $ViewerVerInfo = (Get-Item -LiteralPath $ViewerExePath).VersionInfo
    $ViewerProdVer = [string]$ViewerVerInfo.ProductVersion
    $ViewerSemVer = "$($ViewerVerInfo.ProductMajorPart).$($ViewerVerInfo.ProductMinorPart).$($ViewerVerInfo.ProductBuildPart)"
    if ($ViewerProdVer -ne $Version -and $ViewerSemVer -ne $Version) {
        throw "$ViewerExePath 的 ProductVersion ($ViewerProdVer / $ViewerSemVer) 与根目录 VERSION ($Version) 不一致"
    }
    Write-Output "[OK] Tools3000_Picasa_Viewer.exe ProductVersion verified: $ViewerProdVer"
} elseif ($RequireBinary) {
    throw "已启用 -RequireBinary，但找不到待校验二进制: $ViewerExePath"
} else {
    Write-Output "[INFO] 未检测到预构建二进制 ($ViewerExePath)，已完成全部源码版本单一事实源校验。"
}

# 7. 校验 Rust WindowKit 源码与产物版本一致性
$WindowKitCargoToml = Join-Path $ProjectRoot "src\window_kit\Cargo.toml"
if (Test-Path -LiteralPath $WindowKitCargoToml) {
    $WkCargoRaw = [System.IO.File]::ReadAllText($WindowKitCargoToml, [System.Text.Encoding]::UTF8)
    $WkCargoMatch = [regex]::Match($WkCargoRaw, '(?ms)^\[package\][^\[]*?^version\s*=\s*"([^"]+)"')
    if (-not $WkCargoMatch.Success -or $WkCargoMatch.Groups[1].Value -ne $Version) {
        throw "src/window_kit/Cargo.toml 版本号与根目录 VERSION ($Version) 不一致"
    }
}
$WindowKitExePath = Join-Path $ResolvedBinaryDir "Tools3000_WindowKit.exe"
if (Test-Path -LiteralPath $WindowKitExePath) {
    $WkVerInfo = (Get-Item -LiteralPath $WindowKitExePath).VersionInfo
    $WkProdVer = [string]$WkVerInfo.ProductVersion
    $WkSemVer = "$($WkVerInfo.ProductMajorPart).$($WkVerInfo.ProductMinorPart).$($WkVerInfo.ProductBuildPart)"
    if ($WkProdVer -ne $Version -and $WkSemVer -ne $Version) {
        throw "$WindowKitExePath 的 ProductVersion ($WkProdVer / $WkSemVer) 与根目录 VERSION ($Version) 不一致"
    }
    Write-Output "[OK] Tools3000_WindowKit.exe ProductVersion verified: $WkProdVer"
} elseif ($RequireBinary) {
    throw "已启用 -RequireBinary，但找不到待校验二进制: $WindowKitExePath"
}

Write-Output "[OK] Tools3000 product version verified across VERSION, ui/package.json, and Rust viewer manifests: $Version"
