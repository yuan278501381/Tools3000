#!/usr/bin/env pwsh
<#
.SYNOPSIS
Tools3000 Rust Picasa 看图器独立测试与 Release 构建轨道 (Build-RustViewerTrack)

.DESCRIPTION
供 deploy.ps1 作为独立后台并发轨道启动，与前端 UI 与 C++ CMake 构建零耦合并行运行，
内含工具链目标架构强校验、x64 单元测试与目标架构 Release 增量编译。
#>
param(
    [Parameter(Mandatory = $true)]
    [string]$ViewerProjectDir,
    [Parameter(Mandatory = $true)]
    [ValidateSet("x64", "arm64")]
    [string]$Arch,
    [Parameter(Mandatory = $true)]
    [string]$CargoPath,
    [switch]$SkipTests = $false
)

$ErrorActionPreference = "Stop"
[Console]::OutputEncoding = [System.Text.Encoding]::UTF8
$OutputEncoding = [System.Text.Encoding]::UTF8

$TrackSw = [System.Diagnostics.Stopwatch]::StartNew()
$ViewerRustTarget = if ($Arch -eq "arm64") { "aarch64-pc-windows-msvc" } else { "x86_64-pc-windows-msvc" }
$CargoBinDir = Split-Path -Parent $CargoPath

$RustupCmd = Get-Command "rustup" -ErrorAction SilentlyContinue
if (-not $RustupCmd -and (Test-Path -LiteralPath (Join-Path $CargoBinDir "rustup.exe"))) {
    $RustupCmd = Get-Command (Join-Path $CargoBinDir "rustup.exe") -ErrorAction SilentlyContinue
}
if (-not $RustupCmd -and (Test-Path -LiteralPath (Join-Path $env:USERPROFILE ".cargo\bin\rustup.exe"))) {
    $RustupCmd = Get-Command (Join-Path $env:USERPROFILE ".cargo\bin\rustup.exe") -ErrorAction SilentlyContinue
}

if ($RustupCmd) {
    $InstalledRustTargets = @(& $RustupCmd.Source target list --installed 2>$null | ForEach-Object { $_.Trim() } | Where-Object { $_ })
    if ($LASTEXITCODE -ne 0) {
        throw "无法通过 rustup 查询已安装的 Rust 编译目标（退出码: $LASTEXITCODE）。"
    }
    if ($ViewerRustTarget -notin $InstalledRustTargets) {
        throw "未安装 Rust 目标工具链 '$ViewerRustTarget'（当前已安装: $($InstalledRustTargets -join ', ')）。请先执行: rustup target add $ViewerRustTarget"
    }
    Write-Host "[RUST] [OK] Rust 目标工具链校验通过: $ViewerRustTarget" -ForegroundColor Green
} elseif ($Arch -eq "arm64") {
    $RustcCmd = Get-Command "rustc" -ErrorAction SilentlyContinue
    if (-not $RustcCmd -and (Test-Path -LiteralPath (Join-Path $CargoBinDir "rustc.exe"))) {
        $RustcCmd = Get-Command (Join-Path $CargoBinDir "rustc.exe") -ErrorAction SilentlyContinue
    }
    if (-not $RustcCmd) {
        throw "ARM64 交叉编译未找到 rustup 或 rustc，无法验证 Rust 目标 '$ViewerRustTarget' 是否已安装。"
    }
    $RustSysroot = (& $RustcCmd.Source --print sysroot 2>$null).Trim()
    if ([string]::IsNullOrWhiteSpace($RustSysroot) -or -not (Test-Path -LiteralPath (Join-Path $RustSysroot "lib\rustlib\$ViewerRustTarget"))) {
        throw "未安装 Rust ARM64 目标标准库 '$ViewerRustTarget'（sysroot: $RustSysroot）。请先执行: rustup target add $ViewerRustTarget"
    }
    Write-Host "[RUST] [OK] Rust ARM64 sysroot 目标工具链校验通过: $ViewerRustTarget" -ForegroundColor Green
}

Push-Location $ViewerProjectDir
try {
    if (-not $SkipTests -and $Arch -eq "x64") {
        $TestSw = [System.Diagnostics.Stopwatch]::StartNew()
        Write-Host "[RUST] 运行 Rust Picasa 看图器单元测试 (cargo test --target $ViewerRustTarget)..." -ForegroundColor Cyan
        & $CargoPath test --target $ViewerRustTarget
        if ($LASTEXITCODE -ne 0) {
            throw "Rust Picasa 图片查看器单元测试失败！退出码: $LASTEXITCODE"
        }
        $TestSw.Stop()
        Write-Host ("[RUST] [OK] Rust 单元测试通过 ({0:N2}s)" -f $TestSw.Elapsed.TotalSeconds) -ForegroundColor Green
    }

    $BuildSw = [System.Diagnostics.Stopwatch]::StartNew()
    Write-Host "[RUST] 编译 Rust Picasa 看图器 ($Arch Release, target=$ViewerRustTarget)..." -ForegroundColor Cyan
    & $CargoPath build --release --target $ViewerRustTarget
    if ($LASTEXITCODE -ne 0) {
        throw "Rust Picasa 图片查看器编译失败！退出码: $LASTEXITCODE"
    }
    $BuildSw.Stop()
    $TrackSw.Stop()
    Write-Host ("[RUST] [OK] Rust Picasa 看图器编译完成 (build={0:N2}s, total={1:N2}s)" -f $BuildSw.Elapsed.TotalSeconds, $TrackSw.Elapsed.TotalSeconds) -ForegroundColor Green
} finally {
    Pop-Location
}
