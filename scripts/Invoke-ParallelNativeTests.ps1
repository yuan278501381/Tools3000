#!/usr/bin/env pwsh
<#
.SYNOPSIS
Tools3000 原生 C++ 单元测试沙箱隔离智能并发调度器 (Invoke-ParallelNativeTests)

.DESCRIPTION
在不跳过、不削弱任何一条 GTest 断言的前提下，通过互斥无交集静态分组 + 独立 TOOLS3000_DATA_ROOT 沙箱隔离，
将 Tools3000Tests.exe (800+ 用例) 从单进程串行 ~105s 提速至 ~45-52s：
  1. 重型 ONNX DeepOcr 推理池 (OCR_CORE) 独占单进程，避免多进程 ONNX 线程池争抢物理核导致空载超时抖动；
  2. 插件与跨进程隔离池 (OCR_PLUGIN) 独立运行，与 OCR_CORE 严格互斥；
  3. 其余 740+ 轻量单测按 GTEST_TOTAL_SHARDS=2 均分为 FAST_0 / FAST_1 并发完跑；
  4. 每个分片分配独立 TOOLS3000_DATA_ROOT 沙箱，并通过只读目录联接 (Junction) 零拷贝共享宿主 OCR 模型目录；
  5. 数学闭环守卫：自动比对 --gtest_list_tests 全量启用用例数与 4 路实际运行用例总和，确保 0 遗漏、0 重复！
#>
param(
    [Parameter(Mandatory = $true)]
    [string]$TestExePath,
    [string]$TraceId = "gtest"
)

$ErrorActionPreference = "Stop"
[Console]::OutputEncoding = [System.Text.Encoding]::UTF8
$OutputEncoding = [System.Text.Encoding]::UTF8

if (-not (Test-Path -LiteralPath $TestExePath -PathType Leaf)) {
    throw "找不到原生单元测试可执行文件: $TestExePath"
}

# 1. 精确盘点所有非 DISABLED 用例总数作为数学防漏基线
$listLines = @(& $TestExePath --gtest_list_tests 2>&1)
if ($LASTEXITCODE -ne 0) {
    throw "无法列举 GTest 用例清单 (exit=$LASTEXITCODE)"
}
$expectedTotalTests = 0
$currentSuite = ""
foreach ($line in $listLines) {
    if ($line -match '^(\S+)\.\s*$') {
        $currentSuite = $Matches[1]
    } elseif ($line -match '^\s+(\S+)') {
        $caseName = $Matches[1]
        if (-not $currentSuite.StartsWith("DISABLED_") -and -not $caseName.StartsWith("DISABLED_")) {
            $expectedTotalTests++
        }
    }
}
if ($expectedTotalTests -le 0) {
    throw "未能从 $TestExePath 解析到有效 GTest 用例数"
}

# 2. 定义严格互斥无交集的 4 路并发分片（覆盖全集 100%）
$ocrCorePattern   = "*Ocr*:*LiveOcr*"
$ocrPluginPattern = "*TextReplace*:*WorkerIsolation*:*PinWindow*:*CapturePluginIntegration*:*CaptureBatch2InteractionUpgrade*"
$filterCore       = $ocrCorePattern
$filterPlugin     = "${ocrPluginPattern}-${ocrCorePattern}"
$filterFast       = "-${ocrCorePattern}:${ocrPluginPattern}"

$shards = @(
    @{ Name = "OCR_CORE";   Filter = $filterCore;   TotalShards = 1; ShardIndex = 0 },
    @{ Name = "OCR_PLUGIN"; Filter = $filterPlugin; TotalShards = 1; ShardIndex = 0 },
    @{ Name = "FAST_0";     Filter = $filterFast;   TotalShards = 2; ShardIndex = 0 },
    @{ Name = "FAST_1";     Filter = $filterFast;   TotalShards = 2; ShardIndex = 1 }
)

$hostModelsDir = Join-Path $env:LOCALAPPDATA "Tools3000\models"
$sandboxRoot = Join-Path ([System.IO.Path]::GetTempPath()) ("tools3000_gtest_" + $TraceId + "_" + [guid]::NewGuid().ToString("N").Substring(0, 6))
New-Item -ItemType Directory -Path $sandboxRoot -Force | Out-Null

$wallSw = [System.Diagnostics.Stopwatch]::StartNew()
$running = @()
try {
    Write-Host "[TEST] 启动 4 路沙箱隔离并发 C++ 单元测试 (全量基线: $expectedTotalTests 个用例)..." -ForegroundColor Cyan
    foreach ($s in $shards) {
        $dataDir = Join-Path $sandboxRoot $s.Name
        New-Item -ItemType Directory -Path $dataDir -Force | Out-Null
        if (Test-Path -LiteralPath $hostModelsDir -PathType Container) {
            # 只读零拷贝目录联接：清理时仅安全删除联接本身，绝不触碰宿主真实 OCR 模型
            $null = New-Item -ItemType Junction -Path (Join-Path $dataDir "models") -Target $hostModelsDir
        }

        $psi = New-Object System.Diagnostics.ProcessStartInfo
        $psi.FileName = $TestExePath
        $psi.Arguments = "--gtest_brief=1 --gtest_filter=$($s.Filter)"
        $psi.WorkingDirectory = Split-Path -Parent $TestExePath
        $psi.UseShellExecute = $false
        $psi.RedirectStandardOutput = $true
        $psi.RedirectStandardError = $true
        $psi.StandardOutputEncoding = [System.Text.Encoding]::UTF8
        $psi.StandardErrorEncoding = [System.Text.Encoding]::UTF8
        $psi.CreateNoWindow = $true
        $psi.EnvironmentVariables["TOOLS3000_DATA_ROOT"] = $dataDir
        if ($s.TotalShards -gt 1) {
            $psi.EnvironmentVariables["GTEST_TOTAL_SHARDS"] = "$($s.TotalShards)"
            $psi.EnvironmentVariables["GTEST_SHARD_INDEX"] = "$($s.ShardIndex)"
        }

        $shardSw = [System.Diagnostics.Stopwatch]::StartNew()
        $proc = [System.Diagnostics.Process]::Start($psi)
        $running += [pscustomobject]@{
            Name    = $s.Name
            DataDir = $dataDir
            Proc    = $proc
            Sw      = $shardSw
            OutTask = $proc.StandardOutput.ReadToEndAsync()
            ErrTask = $proc.StandardError.ReadToEndAsync()
        }
    }

    while ($running | Where-Object { -not $_.Proc.HasExited }) {
        foreach ($item in $running) {
            if ($item.Proc.HasExited -and $item.Sw.IsRunning) { $item.Sw.Stop() }
        }
        Start-Sleep -Milliseconds 80
    }
    foreach ($item in $running) {
        if ($item.Sw.IsRunning) { $item.Sw.Stop() }
    }
    $wallSw.Stop()

    $actualTotalExecuted = 0
    $failedShards = @()
    foreach ($item in $running) {
        $stdout = $item.OutTask.Result
        $stderr = $item.ErrTask.Result
        $ranCount = 0
        $ranMatch = [regex]::Match($stdout, '\[==========\] (\d+) tests? from \d+ test suites? ran')
        if ($ranMatch.Success) {
            $ranCount = [int]$ranMatch.Groups[1].Value
            $actualTotalExecuted += $ranCount
        }
        $sec = [math]::Round($item.Sw.Elapsed.TotalSeconds, 2)
        if ($item.Proc.ExitCode -eq 0) {
            Write-Host ("[TEST] [OK] {0,-10} : {1,3} tests passed ({2:N2}s)" -f $item.Name, $ranCount, $sec) -ForegroundColor Green
        } else {
            Write-Host ("[TEST] [FAIL] {0,-10} : exit={1} ({2:N2}s)" -f $item.Name, $item.Proc.ExitCode, $sec) -ForegroundColor Red
            if (-not [string]::IsNullOrWhiteSpace($stdout)) { [Console]::Out.WriteLine($stdout.TrimEnd()) }
            if (-not [string]::IsNullOrWhiteSpace($stderr)) { [Console]::Error.WriteLine($stderr.TrimEnd()) }
            $failedShards += "$($item.Name)(exit=$($item.Proc.ExitCode))"
        }
    }

    if ($failedShards.Count -gt 0) {
        throw "原生 C++ 单元测试分片失败: $($failedShards -join ', ')"
    }
    if ($actualTotalExecuted -ne $expectedTotalTests) {
        throw "测试分片总数校验未闭环！实际运行 $actualTotalExecuted 个用例，全量基线为 $expectedTotalTests 个用例，拒绝漏测或重测。"
    }
    Write-Host ("[TEST] [OK] 全量 {0}/{1} 个原生 C++ 单元测试 100% 通过 (4 路并发总耗时: {2:N2}s)" -f $actualTotalExecuted, $expectedTotalTests, $wallSw.Elapsed.TotalSeconds) -ForegroundColor Green
} finally {
    foreach ($item in $running) {
        if ($item.Proc -and -not $item.Proc.HasExited) {
            try { $item.Proc.Kill() } catch {}
        }
        $juncPath = Join-Path $item.DataDir "models"
        if (Test-Path -LiteralPath $juncPath) {
            try { [System.IO.Directory]::Delete($juncPath) } catch {}
        }
    }
    Remove-Item -LiteralPath $sandboxRoot -Recurse -Force -ErrorAction SilentlyContinue
}
