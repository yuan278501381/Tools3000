[CmdletBinding()]
param(
    [ValidateSet('Start', 'Stop', 'Sample', 'Summarize', 'MeasureColdStart', 'MeasureSearchFirstOpen')]
    [string]$Action = 'Start',
    [string]$OutputDirectory = (Join-Path $PSScriptRoot '..\artifacts\performance-baseline'),
    [ValidateRange(250, 60000)]
    [int]$SampleIntervalMilliseconds = 1000,
    [string]$ExecutablePath = $(
        $t3000 = (Join-Path $PSScriptRoot '..\build\bin\Release\Tools3000.exe')
        if (Test-Path $t3000) { $t3000 } else { (Join-Path $PSScriptRoot '..\build\bin\Release\Tools3000.exe') }
    ),
    [ValidateRange(1, 30)]
    [int]$Iterations = 5
)

$ErrorActionPreference = 'Stop'
$providerGuid = '{92C5838F-961B-4D7D-8C31-38A1E92A137B}'
$sessionPrefix = 'Tools3000PerformanceBaseline'

function Get-BaselinePaths {
    param([string]$Directory)
    $fullDirectory = [IO.Path]::GetFullPath($Directory)
    return [pscustomobject]@{
        Directory = $fullDirectory
        Metadata = Join-Path $fullDirectory 'session.json'
        StopFile = Join-Path $fullDirectory 'stop-sampling'
        Csv = Join-Path $fullDirectory 'process-resources.csv'
        Etl = Join-Path $fullDirectory 'tools3000-performance.etl'
        Machine = Join-Path $fullDirectory 'machine.json'
        Summary = Join-Path $fullDirectory 'summary.json'
        ColdStart = Join-Path $fullDirectory 'cold-start-summary.json'
        SearchFirstOpen = Join-Path $fullDirectory 'search-first-open-summary.json'
        SamplerStdout = Join-Path $fullDirectory 'sampler.stdout.log'
        SamplerStderr = Join-Path $fullDirectory 'sampler.stderr.log'
    }
}

function Assert-BenchmarkHostIsNotRunning {
    $alreadyRunning = @(Get-Process -Name 'Tools3000' -ErrorAction SilentlyContinue)
    if ($alreadyRunning.Count -gt 0) {
        throw '检测到现有 Tools3000 进程；基准拒绝接管或终止用户会话。请完全退出后重试。'
    }
}

function Measure-ColdStart {
    param($Paths, [string]$Program, [int]$RunCount)
    $programPath = [IO.Path]::GetFullPath($Program)
    if (-not (Test-Path -LiteralPath $programPath -PathType Leaf)) {
        throw "找不到 Tools3000 可执行文件：$programPath"
    }
    Assert-BenchmarkHostIsNotRunning
    New-Item -ItemType Directory -Force -Path $Paths.Directory | Out-Null
    Get-MachineMetadata | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath $Paths.Machine -Encoding utf8
    $runs = @()
    for ($index = 1; $index -le $RunCount; ++$index) {
        $snapshot = Join-Path $Paths.Directory ('cold-start-{0:D3}.json' -f $index)
        $outputArgument = '--performance-baseline-output="' + $snapshot.Replace('"', '""') + '"'
        $process = Start-Process -FilePath $programPath -ArgumentList @('--silent', $outputArgument) -PassThru
        if (-not $process.WaitForExit(120000)) {
            # This is the exact process spawned above, never a process selected
            # by name. A hung benchmark must not leave a test host resident.
            Stop-Process -Id $process.Id -Force
            throw "第 $index 次冷启动基准超过 120 秒"
        }
        if ($process.ExitCode -ne 0) { throw "第 $index 次冷启动进程退出码为 $($process.ExitCode)" }
        if (-not (Test-Path -LiteralPath $snapshot -PathType Leaf)) {
            throw "第 $index 次未生成启动快照：$snapshot"
        }
        $document = Get-Content -LiteralPath $snapshot -Raw | ConvertFrom-Json
        $latency = $document.metrics.latencies.'startup.core'.lastMs
        if ($null -eq $latency) { throw "第 $index 次快照缺少 startup.core 延迟" }
        $runs += [pscustomobject]@{ iteration = $index; snapshot = $snapshot; startupCoreMs = [double]$latency }
    }
    [double[]]$values = @($runs | ForEach-Object { $_.startupCoreMs })
    [pscustomobject]@{
        generatedUtc = [DateTime]::UtcNow.ToString('o')
        scenario = 'cold-start'
        executable = $programPath
        iterations = $runs
        startupCoreMs = [pscustomobject]@{
            p50 = Get-Percentile $values 0.50
            p95 = Get-Percentile $values 0.95
            max = ($values | Measure-Object -Maximum).Maximum
        }
        # This scenario records only the in-process startup.core probe. It does
        # not pretend to measure screenshot/search/recording UI readiness.
    } | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $Paths.ColdStart -Encoding utf8
}

function Measure-SearchFirstOpen {
    param($Paths, [string]$Program, [int]$RunCount)
    $programPath = [IO.Path]::GetFullPath($Program)
    if (-not (Test-Path -LiteralPath $programPath -PathType Leaf)) {
        throw "找不到 Tools3000 可执行文件：$programPath"
    }
    Assert-BenchmarkHostIsNotRunning
    New-Item -ItemType Directory -Force -Path $Paths.Directory | Out-Null
    Get-MachineMetadata | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath $Paths.Machine -Encoding utf8
    $runs = @()
    for ($index = 1; $index -le $RunCount; ++$index) {
        $snapshot = Join-Path $Paths.Directory ('search-first-open-{0:D3}.json' -f $index)
        $outputArgument = '--performance-baseline-output="' + $snapshot.Replace('"', '""') + '"'
        # This is an opt-in benchmark-only launch. The host creates the real
        # SearchWindow, records search.hostShow, hides it after one message-pump
        # turn, and exits. It never sends synthetic input to another process.
        $process = Start-Process -FilePath $programPath -ArgumentList @(
            '--silent', $outputArgument, '--performance-baseline-scenario=search-first-open') -PassThru
        if (-not $process.WaitForExit(120000)) {
            Stop-Process -Id $process.Id -Force
            throw "第 $index 次搜索首开基准超过 120 秒"
        }
        if ($process.ExitCode -ne 0) { throw "第 $index 次搜索首开进程退出码为 $($process.ExitCode)" }
        if (-not (Test-Path -LiteralPath $snapshot -PathType Leaf)) {
            throw "第 $index 次未生成搜索首开快照：$snapshot"
        }
        $document = Get-Content -LiteralPath $snapshot -Raw | ConvertFrom-Json
        $metric = $document.metrics.latencies.PSObject.Properties['search.hostShow'].Value
        $latency = $metric.lastMs
        if ($null -eq $latency) { throw "第 $index 次快照缺少 search.hostShow 延迟" }
        $runs += [pscustomobject]@{ iteration = $index; snapshot = $snapshot; searchHostShowMs = [double]$latency }
    }
    [double[]]$values = @($runs | ForEach-Object { $_.searchHostShowMs })
    [pscustomobject]@{
        generatedUtc = [DateTime]::UtcNow.ToString('o')
        scenario = 'search-first-open'
        executable = $programPath
        iterations = $runs
        searchHostShowMs = [pscustomobject]@{
            p50 = Get-Percentile $values 0.50
            p95 = Get-Percentile $values 0.95
            max = ($values | Measure-Object -Maximum).Maximum
        }
        # This is host-window readiness, not a claim that remote search data or
        # WebView rendering has completed. Those need their own scene metrics.
    } | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $Paths.SearchFirstOpen -Encoding utf8
}

function Get-MachineMetadata {
    $os = Get-CimInstance Win32_OperatingSystem -ErrorAction SilentlyContinue
    $cpu = Get-CimInstance Win32_Processor -ErrorAction SilentlyContinue | Select-Object -First 1
    $gpu = @(Get-CimInstance Win32_VideoController -ErrorAction SilentlyContinue | ForEach-Object {
        [pscustomobject]@{ name = $_.Name; driverVersion = $_.DriverVersion; pnpDeviceId = $_.PNPDeviceID }
    })
    $memory = Get-CimInstance Win32_ComputerSystem -ErrorAction SilentlyContinue
    $webViewVersion = $null
    foreach ($registryPath in @(
        'HKLM:\SOFTWARE\Microsoft\EdgeUpdate\Clients\{F1E7A4F2-1F9D-4D09-8B12-D7C6C1EAFBE7}',
        'HKCU:\SOFTWARE\Microsoft\EdgeUpdate\Clients\{F1E7A4F2-1F9D-4D09-8B12-D7C6C1EAFBE7}')) {
        $value = Get-ItemProperty -Path $registryPath -Name pv -ErrorAction SilentlyContinue
        if ($value.pv) { $webViewVersion = [string]$value.pv; break }
    }
    return [pscustomobject]@{
        collectedUtc = [DateTime]::UtcNow.ToString('o')
        os = [pscustomobject]@{ caption = $os.Caption; version = $os.Version; build = $os.BuildNumber }
        cpu = [pscustomobject]@{ name = $cpu.Name; logicalProcessors = $cpu.NumberOfLogicalProcessors }
        memoryBytes = $memory.TotalPhysicalMemory
        gpu = $gpu
        webView2Version = $webViewVersion
        processDpiAwareness = 'recorded by Tools3000 benchmark host when available'
    }
}

function Get-Percentile {
    param([double[]]$Values, [double]$Percentile)
    if (-not $Values -or $Values.Count -eq 0) { return $null }
    $sorted = @($Values | Sort-Object)
    $position = [Math]::Ceiling(($sorted.Count - 1) * $Percentile)
    return $sorted[[Math]::Max(0, [Math]::Min($sorted.Count - 1, $position))]
}

function Write-ResourceSummary {
    param($Paths)
    $samples = @(Import-Csv -LiteralPath $Paths.Csv -ErrorAction SilentlyContinue)
    $byProcess = @($samples | Group-Object processId | ForEach-Object {
        $rows = @($_.Group)
        $private = @($rows | ForEach-Object { [double]$_.privateBytes })
        $working = @($rows | ForEach-Object { [double]$_.workingSetBytes })
        $handles = @($rows | ForEach-Object { [double]$_.handleCount })
        $gdi = @($rows | ForEach-Object { [double]$_.gdiObjectCount })
        $user = @($rows | ForEach-Object { [double]$_.userObjectCount })
        [pscustomobject]@{
            processId = [int]$_.Name
            sampleCount = $rows.Count
            privateBytes = [pscustomobject]@{ p50 = Get-Percentile $private 0.50; p95 = Get-Percentile $private 0.95; max = ($private | Measure-Object -Maximum).Maximum; delta = $private[-1] - $private[0] }
            workingSetBytes = [pscustomobject]@{ p50 = Get-Percentile $working 0.50; p95 = Get-Percentile $working 0.95; max = ($working | Measure-Object -Maximum).Maximum; delta = $working[-1] - $working[0] }
            handleCount = [pscustomobject]@{ p50 = Get-Percentile $handles 0.50; p95 = Get-Percentile $handles 0.95; max = ($handles | Measure-Object -Maximum).Maximum; delta = $handles[-1] - $handles[0] }
            gdiObjectCount = [pscustomobject]@{ delta = $gdi[-1] - $gdi[0] }
            userObjectCount = [pscustomobject]@{ delta = $user[-1] - $user[0] }
        }
    })
    [pscustomobject]@{
        generatedUtc = [DateTime]::UtcNow.ToString('o')
        sourceCsv = $Paths.Csv
        # Latency/frame metrics remain sourced from Tools3000 PerformanceMonitor
        # artifacts. This summary never fabricates absent scene measurements.
        processResources = $byProcess
    } | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $Paths.Summary -Encoding utf8
}

function Start-Sampler {
    param($Paths, [int]$IntervalMilliseconds)

    Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
public static class Tools3000BaselineNative {
    [DllImport("user32.dll", SetLastError = true)]
    public static extern uint GetGuiResources(IntPtr process, uint flags);
}
'@

    while (-not (Test-Path -LiteralPath $Paths.StopFile)) {
        $processes = @(Get-Process -Name 'Tools3000' -ErrorAction SilentlyContinue)
        foreach ($process in $processes) {
            [pscustomobject]@{
                timestampUtc = [DateTime]::UtcNow.ToString('o')
                processId = $process.Id
                privateBytes = $process.PrivateMemorySize64
                workingSetBytes = $process.WorkingSet64
                handleCount = $process.HandleCount
                gdiObjectCount = [Tools3000BaselineNative]::GetGuiResources($process.Handle, 0)
                userObjectCount = [Tools3000BaselineNative]::GetGuiResources($process.Handle, 1)
            } | Export-Csv -LiteralPath $Paths.Csv -NoTypeInformation -Append
        }
        Start-Sleep -Milliseconds $IntervalMilliseconds
    }
}

$paths = Get-BaselinePaths -Directory $OutputDirectory
switch ($Action) {
    'Sample' {
        Start-Sampler -Paths $paths -IntervalMilliseconds $SampleIntervalMilliseconds
        exit 0
    }
    'Summarize' {
        Write-ResourceSummary -Paths $paths
        Write-Host "资源摘要已写入：$($paths.Summary)"
        exit 0
    }
    'MeasureColdStart' {
        Measure-ColdStart -Paths $paths -Program $ExecutablePath -RunCount $Iterations
        Write-Host "冷启动基线已写入：$($paths.ColdStart)"
        exit 0
    }
    'MeasureSearchFirstOpen' {
        Measure-SearchFirstOpen -Paths $paths -Program $ExecutablePath -RunCount $Iterations
        Write-Host "搜索首开基线已写入：$($paths.SearchFirstOpen)"
        exit 0
    }
    'Start' {
        if (Test-Path -LiteralPath $paths.Metadata) {
            throw "已有 Tools3000 基线会话元数据：$($paths.Metadata)。请先执行 -Action Stop，避免接管未知会话。"
        }
        New-Item -ItemType Directory -Force -Path $paths.Directory | Out-Null
        Get-MachineMetadata | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath $paths.Machine -Encoding utf8
        Remove-Item -LiteralPath $paths.StopFile -Force -ErrorAction SilentlyContinue

        # A fixed machine-wide logman session name makes concurrent collections
        # unsafe: a failed create could otherwise delete another run's trace.
        # The generated name is persisted in metadata and Stop only owns that
        # exact session.
        $sessionName = "$sessionPrefix-$PID-$([guid]::NewGuid().ToString('N'))"
        $etwActive = $false
        $etwSessionCreated = $false
        try {
            # Create/start only this fixed, documented session. The TraceLogging
            # provider is best-effort in the host; logman can still start before
            # Tools3000 registers it.
            & logman create trace $sessionName -o $paths.Etl -p $providerGuid 0xFFFFFFFFFFFFFFFF 0xFF | Out-Null
            if ($LASTEXITCODE -ne 0) { throw "logman create failed ($LASTEXITCODE)" }
            $etwSessionCreated = $true
            & logman start $sessionName -ets | Out-Null
            if ($LASTEXITCODE -ne 0) { throw "logman start failed ($LASTEXITCODE)" }
            $etwActive = $true
        } catch {
            # A non-elevated or policy-restricted machine may reject ETW. The
            # process-resource CSV is still useful and the metadata makes the
            # missing trace explicit instead of silently pretending it exists.
            Write-Warning "无法启动 ETW，会继续采集资源 CSV：$($_.Exception.Message)"
            if ($etwSessionCreated) { & logman delete $sessionName 2>$null | Out-Null }
        }

        try {

            # Start-Process joins ArgumentList into a Windows command line. Quote
            # the two filesystem arguments explicitly so a custom directory such
            # as "D:\Performance Results" remains one argument in the sampler.
            $quotedScriptPath = '"' + $PSCommandPath.Replace('"', '""') + '"'
            $quotedOutputDirectory = '"' + $paths.Directory.Replace('"', '""') + '"'
            # pwsh preserves UTF-8 source encoding; Windows PowerShell 5.1 can
            # misparse a UTF-8 Chinese-language script without a BOM.
            $samplerShell = if (Get-Command pwsh.exe -ErrorAction SilentlyContinue) { 'pwsh.exe' } else { 'powershell.exe' }
            $sampler = Start-Process -FilePath $samplerShell -WindowStyle Hidden -PassThru `
                -RedirectStandardOutput $paths.SamplerStdout -RedirectStandardError $paths.SamplerStderr -ArgumentList @(
                '-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', $quotedScriptPath,
                '-Action', 'Sample', '-OutputDirectory', $quotedOutputDirectory,
                '-SampleIntervalMilliseconds', $SampleIntervalMilliseconds)
            [pscustomobject]@{
                sessionName = $sessionName
                samplerProcessId = $sampler.Id
                startedUtc = [DateTime]::UtcNow.ToString('o')
                etlPath = $paths.Etl
                csvPath = $paths.Csv
                machinePath = $paths.Machine
                summaryPath = $paths.Summary
                providerGuid = $providerGuid
                etwActive = $etwActive
                samplerStdout = $paths.SamplerStdout
                samplerStderr = $paths.SamplerStderr
            } | ConvertTo-Json | Set-Content -LiteralPath $paths.Metadata -Encoding utf8
        } catch {
            if ($etwActive) { & logman stop $sessionName -ets 2>$null | Out-Null }
            if ($etwSessionCreated) { & logman delete $sessionName 2>$null | Out-Null }
            throw
        }
        Write-Host "基线采集已启动：$($paths.Directory)"
        Write-Host '现在按 docs/performance-baseline.md 执行场景；结束时运行同一脚本 -Action Stop。'
        exit 0
    }
    'Stop' {
        if (-not (Test-Path -LiteralPath $paths.Metadata)) {
            throw "找不到本脚本创建的会话元数据：$($paths.Metadata)"
        }
        $metadata = Get-Content -LiteralPath $paths.Metadata -Raw | ConvertFrom-Json
        if ([string]::IsNullOrWhiteSpace($metadata.sessionName) -or
            -not $metadata.sessionName.StartsWith("$sessionPrefix-", [StringComparison]::Ordinal)) {
            throw '会话元数据不匹配，拒绝停止未知 ETW 会话。'
        }
        $sessionName = [string]$metadata.sessionName
        New-Item -ItemType File -Force -Path $paths.StopFile | Out-Null
        $sampler = Get-Process -Id $metadata.samplerProcessId -ErrorAction SilentlyContinue
        if ($sampler) {
            if (-not $sampler.WaitForExit(5000)) {
                Stop-Process -Id $sampler.Id -Force
            }
        }
        if ($metadata.etwActive) {
            & logman stop $sessionName -ets | Out-Null
            if ($LASTEXITCODE -ne 0) { throw "logman stop failed ($LASTEXITCODE)" }
            & logman delete $sessionName | Out-Null
            if ($LASTEXITCODE -ne 0) { throw "logman delete failed ($LASTEXITCODE)" }
        }
        Write-ResourceSummary -Paths $paths
        Remove-Item -LiteralPath $paths.Metadata -Force
        Write-Host "基线采集已停止：ETW=$($metadata.etwActive)；资源样本=$($paths.Csv)"
        exit 0
    }
}
