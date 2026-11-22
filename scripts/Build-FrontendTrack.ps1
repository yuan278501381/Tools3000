#!/usr/bin/env pwsh
<#
.SYNOPSIS
Tools3000 前端高速确定性构建与全量质量门禁轨道 (Build-FrontendTrack)

.DESCRIPTION
设计目标：在 100% 保留全部质量门禁 (ESLint + 6 重静态规范门禁 + Vitest 全量单测 + tsc -b + vite build + 生产 HTML 校验)
的前提下，彻底消除冗余计算并最大化并发：
  1. Lockfile 依赖树规范化 SHA-256 哨兵：仅当依赖树真实变更、node_modules 缺失或 -CleanBuild 时才运行 npm ci，
     其余迭代 0.05s 跳过并完好保留 .eslintcache 与 tsconfig.app.tsbuildinfo 增量缓存；
  2. 轨内并行：并发拉起 npm run lint (ESLint 缓存 + 6 重并发静态门禁) 与 npm run test (Vitest 全量单测)；
  3. 零重复扫描：门禁全绿后直接调用 npm run build:bundle，杜绝此前 npm run build 重复执行 6 重静态门禁的纯浪费。
#>
param(
    [Parameter(Mandatory = $true)]
    [string]$UiDir,
    [switch]$Quick = $false,
    [switch]$CleanBuild = $false
)

$ErrorActionPreference = "Stop"
[Console]::OutputEncoding = [System.Text.Encoding]::UTF8
$OutputEncoding = [System.Text.Encoding]::UTF8

function Get-NpmDependencyTreeHash {
    param([Parameter(Mandatory = $true)][string]$LockFilePath)
    if (-not (Test-Path -LiteralPath $LockFilePath)) {
        return ""
    }
    $raw = [System.IO.File]::ReadAllText($LockFilePath, [System.Text.Encoding]::UTF8)
    # 剥离顶层与 packages[""] 中随发版变动的项目自身 version 字段，仅对真实依赖树做确定性哈希，
    # 避免每次版本号递增时无谓销毁 node_modules / .eslintcache / tsbuildinfo。
    $normalized = $raw
    $verMatches = [regex]::Matches($raw, '(?m)^(\s{2,6}"version"\s*:\s*")[^"]+(")')
    $toStrip = @()
    for ($i = 0; $i -lt [Math]::Min(2, $verMatches.Count); $i++) {
        if ($verMatches[$i].Index -lt 600) {
            $toStrip += $verMatches[$i]
        }
    }
    for ($j = $toStrip.Count - 1; $j -ge 0; $j--) {
        $m = $toStrip[$j]
        $normalized = $normalized.Substring(0, $m.Index) + $m.Groups[1].Value + "__SELF__" + $m.Groups[2].Value + $normalized.Substring($m.Index + $m.Length)
    }
    $sha = [System.Security.Cryptography.SHA256]::Create()
    try {
        $bytes = [System.Text.Encoding]::UTF8.GetBytes($normalized)
        $hashBytes = $sha.ComputeHash($bytes)
        return ([System.BitConverter]::ToString($hashBytes) -replace '-', '').ToLowerInvariant()
    } finally {
        $sha.Dispose()
    }
}

function Invoke-NpmStepParallel {
    param(
        [Parameter(Mandatory = $true)][string]$WorkingDir,
        [Parameter(Mandatory = $true)][string[]]$Scripts
    )
    $npmCmd = (Get-Command "npm.cmd" -ErrorAction SilentlyContinue)
    $exePath = if ($npmCmd) { $npmCmd.Source } else { "cmd.exe" }

    $procs = @()
    foreach ($scriptName in $Scripts) {
        $psi = New-Object System.Diagnostics.ProcessStartInfo
        $psi.WorkingDirectory = $WorkingDir
        if ($npmCmd) {
            $psi.FileName = $exePath
            $psi.Arguments = "run $scriptName"
        } else {
            $psi.FileName = "cmd.exe"
            $psi.Arguments = "/c npm run $scriptName"
        }
        $psi.UseShellExecute = $false
        $psi.RedirectStandardOutput = $true
        $psi.RedirectStandardError = $true
        $psi.StandardOutputEncoding = [System.Text.Encoding]::UTF8
        $psi.StandardErrorEncoding = [System.Text.Encoding]::UTF8
        $psi.CreateNoWindow = $true

        $sw = [System.Diagnostics.Stopwatch]::StartNew()
        $p = [System.Diagnostics.Process]::Start($psi)
        $procs += [pscustomobject]@{
            Name    = $scriptName
            Proc    = $p
            Sw      = $sw
            OutTask = $p.StandardOutput.ReadToEndAsync()
            ErrTask = $p.StandardError.ReadToEndAsync()
        }
    }

    while ($procs | Where-Object { -not $_.Proc.HasExited }) {
        foreach ($item in $procs) {
            if ($item.Proc.HasExited -and $item.Sw.IsRunning) { $item.Sw.Stop() }
        }
        Start-Sleep -Milliseconds 60
    }
    foreach ($item in $procs) {
        if ($item.Sw.IsRunning) { $item.Sw.Stop() }
    }

    $failed = @()
    foreach ($item in $procs) {
        $sec = [math]::Round($item.Sw.Elapsed.TotalSeconds, 2)
        Write-Host "[UI] --- npm run $($item.Name) (${sec}s, exit=$($item.Proc.ExitCode)) ---" -ForegroundColor Cyan
        $outText = $item.OutTask.Result
        $errText = $item.ErrTask.Result
        if (-not [string]::IsNullOrWhiteSpace($outText)) {
            [Console]::Out.WriteLine($outText.TrimEnd())
        }
        if (-not [string]::IsNullOrWhiteSpace($errText)) {
            [Console]::Error.WriteLine($errText.TrimEnd())
        }
        if ($item.Proc.ExitCode -ne 0) {
            $failed += "$($item.Name)(exit=$($item.Proc.ExitCode))"
        }
    }
    if ($failed.Count -gt 0) {
        throw "前端并行门禁失败: $($failed -join ', ')"
    }
}

$TrackSw = [System.Diagnostics.Stopwatch]::StartNew()
# 规范化磁盘路径盘符大小写（Windows 下 Vite/Vitest 4 要求 cwd 盘符大写与模块路径一致）
$ResolvedUiPath = (Resolve-Path -LiteralPath $UiDir).Path
if ($ResolvedUiPath.Length -ge 2 -and $ResolvedUiPath[1] -eq ':') {
    $ResolvedUiPath = $ResolvedUiPath.Substring(0, 1).ToUpperInvariant() + $ResolvedUiPath.Substring(1)
}
$UiDir = $ResolvedUiPath

Push-Location $UiDir
try {
    $NodeModulesDir = Join-Path $UiDir "node_modules"
    $LockFile = Join-Path $UiDir "package-lock.json"
    $StampFile = Join-Path $NodeModulesDir ".tools3000-deps.sha256"
    $CurrentDepsHash = Get-NpmDependencyTreeHash -LockFilePath $LockFile
    $SavedDepsHash = if (Test-Path -LiteralPath $StampFile) { (Get-Content -LiteralPath $StampFile -Raw).Trim() } else { "" }

    $NeedNpmCi = $CleanBuild -or (-not (Test-Path -LiteralPath $NodeModulesDir)) -or
                 ((-not $Quick) -and (-not [string]::IsNullOrEmpty($CurrentDepsHash)) -and ($SavedDepsHash -ne $CurrentDepsHash))

    if ($NeedNpmCi) {
        $CiSw = [System.Diagnostics.Stopwatch]::StartNew()
        Write-Host "[UI] 依赖锁文件已变更或首次冷建，执行 npm ci --prefer-offline --no-audit..." -ForegroundColor Cyan
        npm ci --prefer-offline --no-audit
        if ($LASTEXITCODE -ne 0) { throw "npm ci 失败，退出码: $LASTEXITCODE" }
        if (-not [string]::IsNullOrEmpty($CurrentDepsHash) -and (Test-Path -LiteralPath $NodeModulesDir)) {
            [System.IO.File]::WriteAllText($StampFile, $CurrentDepsHash, [System.Text.UTF8Encoding]::new($false))
        }
        $CiSw.Stop()
        Write-Host ("[UI] [OK] npm ci 完成 ({0:N2}s)" -f $CiSw.Elapsed.TotalSeconds) -ForegroundColor Green
    } else {
        Write-Host "[UI] [OK] package-lock.json 依赖树指纹一致，0ms 复用 node_modules / .eslintcache / tsbuildinfo" -ForegroundColor Green
    }

    # 三路并发执行 lint (ESLint 缓存 + 6 重并发静态门禁) || test (Vitest 全量单测) || typecheck (tsc -b 增量类型检查)
    $GateSw = [System.Diagnostics.Stopwatch]::StartNew()
    Write-Host "[UI] 三路并发执行 lint (ESLint + 6 重静态门禁) || test (Vitest 全量单测) || typecheck (tsc -b)..." -ForegroundColor Cyan
    Invoke-NpmStepParallel -WorkingDir $UiDir -Scripts @("lint", "test", "typecheck")
    $GateSw.Stop()
    Write-Host ("[UI] [OK] 三路并发 lint + test + typecheck 全部通过 ({0:N2}s)" -f $GateSw.Elapsed.TotalSeconds) -ForegroundColor Green

    # 门禁与类型检查均已全绿，直接极速执行 vite build + HTML 生产门禁 (~1s)
    $BundleSw = [System.Diagnostics.Stopwatch]::StartNew()
    Write-Host "[UI] 执行 npm run vite:build (vite build + HTML 生产门禁)..." -ForegroundColor Cyan
    npm run vite:build
    if ($LASTEXITCODE -ne 0) { throw "npm run vite:build 失败，退出码: $LASTEXITCODE" }
    $BundleSw.Stop()
    $TrackSw.Stop()
    Write-Host ("[UI] [OK] 前端轨道全量门禁与打包完成 (vite={0:N2}s, total={1:N2}s)" -f $BundleSw.Elapsed.TotalSeconds, $TrackSw.Elapsed.TotalSeconds) -ForegroundColor Green
} finally {
    Pop-Location
}
