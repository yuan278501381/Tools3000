# -----------------------------------------------------------------------------
# test_memory_lifecycle_e2e.ps1 — Tools3000 全进程树资源与功能闭环实机压测门禁
# -----------------------------------------------------------------------------
# 验证项:
#   [1/5] 冷启动隔离静默基线 + /DELAYLOAD 14 个重型多媒体/AI DLL 零预载门禁
#   [2/5] 隐藏态 WebView2 (Tray/Search) 挂起零 CPU 门禁（5 秒窗口增量采样）
#   [3/5] 文件搜索 & 文档内容全文搜索跨进程 E2E (含 DEMAND_START + 服务内存观测)
#   [4/5] 截图 + 标注 + 贴图 + 录屏 + 演播室 E2E (同步验证 /DELAYLOAD 按需加载无损)
#   [5/5] 全局鼠标手势套件 E2E (验证全功能 0 回退)
# -----------------------------------------------------------------------------
param(
    [string]$ExePath = "",
    [switch]$SkipSubSuites = $false
)

[Console]::OutputEncoding = [System.Text.Encoding]::UTF8
$ErrorActionPreference = "Stop"

$ProjectRoot = Split-Path -Parent $PSScriptRoot
if ([string]::IsNullOrWhiteSpace($ExePath)) {
    $c1 = Join-Path $ProjectRoot "build\bin\Release\Tools3000.exe"
    $c2 = Join-Path $ProjectRoot "deploy_dist\Tools3000.exe"
    if (Test-Path -LiteralPath $c1) { $ExePath = $c1 }
    elseif (Test-Path -LiteralPath $c2) { $ExePath = $c2 }
    else { throw "未找到 Tools3000.exe，请先编译 Release。" }
}
$ExePath = (Resolve-Path -LiteralPath $ExePath).Path
$BaselineScript = Join-Path $PSScriptRoot "collect-performance-baseline.ps1"

Write-Host "===============================================================================" -ForegroundColor Cyan
Write-Host " Tools3000 全进程树内存体检 + /DELAYLOAD 门禁 + 全功能闭环实机压测 " -ForegroundColor Cyan
Write-Host "===============================================================================" -ForegroundColor Cyan
Write-Host "[INFO] 目标程序: $ExePath"

$SandboxRoot = Join-Path $ProjectRoot "build\memory-lifecycle-e2e-$PID"
$SandboxLocal = Join-Path $SandboxRoot "LocalAppData"
$SandboxRoaming = Join-Path $SandboxRoot "AppData"
$SandboxData = Join-Path $SandboxRoot "Tools3000Data"
New-Item -ItemType Directory -Force -Path $SandboxLocal, $SandboxRoaming, $SandboxData | Out-Null

$proc = $null
$report = [ordered]@{
    generatedUtc = [DateTime]::UtcNow.ToString('o')
    executable = $ExePath
    coldStart = $null
    coldModuleGate = $null
    hiddenCpuCheck = $null
    suites = [ordered]@{}
}

try {
    Write-Host "`n[1/5] 启动隔离沙箱静默实例并等待 12 秒稳态..." -ForegroundColor Yellow
    $psi = [System.Diagnostics.ProcessStartInfo]::new()
    $psi.FileName = $ExePath
    $psi.WorkingDirectory = (Split-Path -Parent $ExePath)
    $psi.UseShellExecute = $false
    $psi.Arguments = "--silent --no-elevate --force-portable-search-service --lifecycle-test-instance"
    $psi.EnvironmentVariables["LOCALAPPDATA"] = $SandboxLocal
    $psi.EnvironmentVariables["APPDATA"] = $SandboxRoaming
    $psi.EnvironmentVariables["TOOLS3000_DATA_ROOT"] = $SandboxData
    $psi.EnvironmentVariables["TOOLS3000_TEST_MODE"] = "1"
    $proc = [System.Diagnostics.Process]::Start($psi)
    Start-Sleep -Seconds 12
    if (-not $proc -or $proc.HasExited) {
        throw "隔离实例在冷启动阶段异常退出，ExitCode=$($proc.ExitCode)"
    }

    $snap1Raw = & pwsh -NoProfile -ExecutionPolicy Bypass -File $BaselineScript -Action SnapshotProcessTree -TargetPid $proc.Id
    $snap1 = $snap1Raw | ConvertFrom-Json
    $report.coldStart = $snap1

    Write-Host "[INFO] 冷启动静默 12s 全进程树快照 (PID=$($proc.Id)):" -ForegroundColor Cyan
    foreach ($g in $snap1.groups) {
        if ($g.role -eq 'search-service') { continue }
        Write-Host ("  - {0,-18} count={1,-2} WS={2,6:N2} MB  Private={3,6:N2} MB  CPU={4,6:N3}s" -f $g.role, $g.count, $g.workingSetMB, $g.privateMB, $g.cpuSec)
    }
    Write-Host ("  => 进程树总计 (不含本机常驻搜索服务): count={0}, WS={1:N2} MB, Private={2:N2} MB" -f $snap1.totalWithoutService.count, $snap1.totalWithoutService.workingSetMB, $snap1.totalWithoutService.privateMB) -ForegroundColor Green

    $modRaw = & pwsh -NoProfile -ExecutionPolicy Bypass -File $BaselineScript -Action CheckColdModules -TargetPid $proc.Id
    $modExit = $LASTEXITCODE
    $modCheck = $modRaw | ConvertFrom-Json
    $report.coldModuleGate = $modCheck
    if ($modExit -ne 0 -or -not $modCheck.passed) {
        throw "[ERROR] 冷启动 /DELAYLOAD 门禁失败！违规预载 DLL: $($modCheck.violations -join ', ')"
    }
    Write-Host ("[OK] 冷启动 /DELAYLOAD 门禁通过：已检测 {0} 个重型多媒体/AI DLL，0 违规预载（主进程总模块数={1}）" -f $modCheck.checkedDeferredDllCount, $modCheck.totalLoadedModules) -ForegroundColor Green

    Write-Host "`n[2/5] 采样 6 秒隐藏态 WebView2 渲染器 CPU 增量..." -ForegroundColor Yellow
    Start-Sleep -Seconds 6
    $snap2Raw = & pwsh -NoProfile -ExecutionPolicy Bypass -File $BaselineScript -Action SnapshotProcessTree -TargetPid $proc.Id
    $snap2 = $snap2Raw | ConvertFrom-Json

    $cpuBefore = @{}
    foreach ($p in $snap1.processes) { $cpuBefore[[int]$p.pid] = [double]$p.cpuSec }
    $rendererDeltas = @()
    foreach ($p in $snap2.processes) {
        if ($p.role -eq 'wv2:renderer') {
            $prevCpu = if ($cpuBefore.ContainsKey([int]$p.pid)) { $cpuBefore[[int]$p.pid] } else { 0.0 }
            $delta = [math]::Round([double]$p.cpuSec - $prevCpu, 3)
            $rendererDeltas += [pscustomobject]@{ pid = $p.pid; deltaCpuSec = $delta; workingSetMB = $p.workingSetMB; privateMB = $p.privateMB }
            Write-Host ("  - Hidden wv2:renderer PID={0}: 6s CPU delta={1:N3}s, WS={2:N2} MB, Private={3:N2} MB" -f $p.pid, $delta, $p.workingSetMB, $p.privateMB)
        }
    }
    $maxRendererDelta = if ($rendererDeltas.Count -gt 0) { ($rendererDeltas | Measure-Object deltaCpuSec -Maximum).Maximum } else { 0.0 }
    $report.hiddenCpuCheck = [pscustomobject]@{
        windowSec = 6
        rendererCount = $rendererDeltas.Count
        maxDeltaCpuSec = $maxRendererDelta
        renderers = $rendererDeltas
    }
    if ($maxRendererDelta -gt 0.5) {
        throw "[ERROR] 隐藏态 WebView2 renderer 在 6 秒内消耗 CPU ${maxRendererDelta}s (>0.5s)，挂起或动画暂停未生效！"
    }
    Write-Host ("[OK] 隐藏态 WebView2 零 CPU 门禁通过：最大单 renderer 6s 增量 = {0:N3}s (阈值 <= 0.500s)" -f $maxRendererDelta) -ForegroundColor Green
}
finally {
    if ($proc -and -not $proc.HasExited) {
        Stop-Process -Id $proc.Id -Force -ErrorAction SilentlyContinue
        Start-Sleep -Milliseconds 500
    }
    Remove-Item -LiteralPath $SandboxRoot -Recurse -Force -ErrorAction SilentlyContinue
}

if (-not $SkipSubSuites) {
    Write-Host "`n[3/5] 执行文件搜索 & 文档内容全文搜索 E2E (test_search_content_e2e.ps1)..." -ForegroundColor Yellow
    & pwsh -NoProfile -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot "test_search_content_e2e.ps1") -ExePath $ExePath
    if ($LASTEXITCODE -ne 0) { throw "文件搜索与内容搜索 E2E 失败 (ExitCode=$LASTEXITCODE)" }
    $report.suites.searchContentE2E = "PASS"

    Write-Host "`n[4/5] 执行截图/标注/贴图/录屏/演播室 E2E (test_capture_recording_e2e.ps1，同步验证 /DELAYLOAD 按需加载)..." -ForegroundColor Yellow
    & pwsh -NoProfile -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot "test_capture_recording_e2e.ps1") -ExePath $ExePath
    if ($LASTEXITCODE -ne 0) { throw "截图与录屏演播室 E2E 失败 (ExitCode=$LASTEXITCODE)" }
    $report.suites.captureRecordingE2E = "PASS"

    Write-Host "`n[5/5] 执行鼠标手势套件 E2E (test_gesture_suite_e2e.ps1)..." -ForegroundColor Yellow
    & pwsh -NoProfile -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot "test_gesture_suite_e2e.ps1") -ExePath $ExePath
    if ($LASTEXITCODE -ne 0) { throw "鼠标手势套件 E2E 失败 (ExitCode=$LASTEXITCODE)" }
    $report.suites.gestureSuiteE2E = "PASS"
}

$OutDir = Join-Path $ProjectRoot "artifacts\performance-baseline"
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null
$OutJson = Join-Path $OutDir "memory-lifecycle-e2e-report.json"
$report | ConvertTo-Json -Depth 10 | Set-Content -LiteralPath $OutJson -Encoding utf8
Write-Host "`n[OK] 全进程树内存与全功能闭环压测全部通过！报告已写入: $OutJson" -ForegroundColor Green
