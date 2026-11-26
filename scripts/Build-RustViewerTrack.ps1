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
    [string]$CliProjectDir = "",
    [string]$WindowKitProjectDir = "",
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
    Write-Host ("[RUST] [OK] Rust Picasa 看图器编译完成 (build={0:N2}s)" -f $BuildSw.Elapsed.TotalSeconds) -ForegroundColor Green
} finally {
    Pop-Location
}

if (-not [string]::IsNullOrWhiteSpace($CliProjectDir) -and (Test-Path -LiteralPath (Join-Path $CliProjectDir "Cargo.toml"))) {
    Push-Location $CliProjectDir
    try {
        if (-not $SkipTests -and $Arch -eq "x64") {
            $CliTestSw = [System.Diagnostics.Stopwatch]::StartNew()
            Write-Host "[RUST] 运行 Rust CLI / MCP 单元测试 (cargo test --target $ViewerRustTarget)..." -ForegroundColor Cyan
            & $CargoPath test --target $ViewerRustTarget
            if ($LASTEXITCODE -ne 0) {
                throw "Rust CLI / MCP 单元测试失败！退出码: $LASTEXITCODE"
            }
            $CliTestSw.Stop()
            Write-Host ("[RUST] [OK] Rust CLI / MCP 单元测试通过 ({0:N2}s)" -f $CliTestSw.Elapsed.TotalSeconds) -ForegroundColor Green
        }

        $CliBuildSw = [System.Diagnostics.Stopwatch]::StartNew()
        Write-Host "[RUST] 编译 Rust CLI / MCP 工具 (tools3000-cli.exe, $Arch Release)..." -ForegroundColor Cyan
        & $CargoPath build --release --target $ViewerRustTarget
        if ($LASTEXITCODE -ne 0) {
            throw "Rust CLI / MCP 工具编译失败！退出码: $LASTEXITCODE"
        }
        $CliBuildSw.Stop()
        Write-Host ("[RUST] [OK] Rust CLI / MCP 编译完成 (build={0:N2}s)" -f $CliBuildSw.Elapsed.TotalSeconds) -ForegroundColor Green
    } finally {
        Pop-Location
    }
}

$ClipboardProjectDir = Join-Path (Split-Path -Parent $ViewerProjectDir) "clipboard\rust_clipboard"
if (-not $SkipTests -and $Arch -eq "x64" -and (Test-Path -LiteralPath (Join-Path $ClipboardProjectDir "Cargo.toml"))) {
    Push-Location $ClipboardProjectDir
    try {
        $ClipTestSw = [System.Diagnostics.Stopwatch]::StartNew()
        Write-Host "[RUST] 运行 Rust 持久化剪贴板内核单元测试 (cargo test --target $ViewerRustTarget)..." -ForegroundColor Cyan
        & $CargoPath test --target $ViewerRustTarget
        if ($LASTEXITCODE -ne 0) {
            throw "Rust 剪贴板内核单元测试失败！退出码: $LASTEXITCODE"
        }
        $ClipTestSw.Stop()
        Write-Host ("[RUST] [OK] Rust 剪贴板内核单元测试通过 ({0:N2}s)" -f $ClipTestSw.Elapsed.TotalSeconds) -ForegroundColor Green
    } finally {
        Pop-Location
    }
}

$CapsProjectDir = Join-Path (Split-Path -Parent $ViewerProjectDir) "caps_layer\rust_caps"
if (-not $SkipTests -and $Arch -eq "x64" -and (Test-Path -LiteralPath (Join-Path $CapsProjectDir "Cargo.toml"))) {
    Push-Location $CapsProjectDir
    try {
        $CapsTestSw = [System.Diagnostics.Stopwatch]::StartNew()
        Write-Host "[RUST] 运行 Rust CapsLayer 引导键状态机单元测试 (cargo test --target $ViewerRustTarget)..." -ForegroundColor Cyan
        & $CargoPath test --target $ViewerRustTarget
        if ($LASTEXITCODE -ne 0) {
            throw "Rust CapsLayer 单元测试失败！退出码: $LASTEXITCODE"
        }
        $CapsTestSw.Stop()
        Write-Host ("[RUST] [OK] Rust CapsLayer 单元测试通过 ({0:N2}s)" -f $CapsTestSw.Elapsed.TotalSeconds) -ForegroundColor Green
    } finally {
        Pop-Location
    }
}

if ([string]::IsNullOrWhiteSpace($WindowKitProjectDir)) {
    $WindowKitProjectDir = Join-Path (Split-Path -Parent $ViewerProjectDir) "window_kit"
}
if (Test-Path -LiteralPath (Join-Path $WindowKitProjectDir "Cargo.toml")) {
    Push-Location $WindowKitProjectDir
    try {
        if (-not $SkipTests -and $Arch -eq "x64") {
            $WkTestSw = [System.Diagnostics.Stopwatch]::StartNew()
            Write-Host "[RUST] 运行 Rust 窗口管理引擎单元测试 (cargo test --target $ViewerRustTarget)..." -ForegroundColor Cyan
            & $CargoPath test --target $ViewerRustTarget
            if ($LASTEXITCODE -ne 0) {
                throw "Rust 窗口管理引擎单元测试失败！退出码: $LASTEXITCODE"
            }
            $WkTestSw.Stop()
            Write-Host ("[RUST] [OK] Rust 窗口管理引擎单元测试通过 ({0:N2}s)" -f $WkTestSw.Elapsed.TotalSeconds) -ForegroundColor Green
        }

        $WkBuildSw = [System.Diagnostics.Stopwatch]::StartNew()
        Write-Host "[RUST] 编译 Rust 窗口管理引擎 (Tools3000_WindowKit.exe, $Arch Release)..." -ForegroundColor Cyan
        & $CargoPath build --release --target $ViewerRustTarget
        if ($LASTEXITCODE -ne 0) {
            throw "Rust 窗口管理引擎编译失败！退出码: $LASTEXITCODE"
        }
        $WkBuildSw.Stop()
        Write-Host ("[RUST] [OK] Rust 窗口管理引擎编译完成 (build={0:N2}s)" -f $WkBuildSw.Elapsed.TotalSeconds) -ForegroundColor Green
    } finally {
        Pop-Location
    }
}

$TrackSw.Stop()
Write-Host ("[RUST] [OK] Rust 全量并行轨道完成 (total={0:N2}s)" -f $TrackSw.Elapsed.TotalSeconds) -ForegroundColor Green
