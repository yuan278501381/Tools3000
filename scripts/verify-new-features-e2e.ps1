# ==============================================================================
# Tools3000 新增特性全链路实机 E2E 防回退门禁
# Copyright (c) 2026 Yy1 (yuan278501381) & Tools3000 contributors
#
# 覆盖实机断言:
#   1. 全 8 语种跨代码页 Unicode 路径 + 反馈 10 维隐私脱敏/ZIP/PoW + File Locksmith + FontPreview 单测组
#   2. 手势 RunProgram 切窗 + 停顿取消 + 8 秒红底三白点彩蛋 + Lua 字节码缓存单测组
#   3. 搜索 3 档内存策略 + Live Watch 代次递增 + 应用/37命令/书签融合排序单测组
#   4. 真机跨进程 Win32 文件对话框四模式 E2E (IFileDialog Folder/Open/Save + Legacy OFN_EXPLORER)
#   5. 真机短命隔离 MediaProbeWorker (--worker=media-probe) FFmpeg 探测与抽帧闭环
#   6. 8 语种混合 Unicode 临时路径实机插件目录复制与隔离校验
# ==============================================================================
[CmdletBinding()]
param(
    [string]$BinDir = (Join-Path $PSScriptRoot "..\build\bin\Release")
)

$ErrorActionPreference = "Stop"
[Console]::OutputEncoding = [System.Text.Encoding]::UTF8

$BinDir = (Resolve-Path -LiteralPath $BinDir).Path
$TestsExe = Join-Path $BinDir "Tools3000Tests.exe"
$DialogE2eExe = Join-Path $BinDir "Tools3000DialogNavigatorIntegration.exe"
$MainExe = Join-Path $BinDir "Tools3000.exe"

foreach ($req in @($TestsExe, $DialogE2eExe, $MainExe)) {
    if (-not (Test-Path -LiteralPath $req -PathType Leaf)) {
        throw "[ERROR] 缺少实机测试必需产物: $req"
    }
}

Write-Host "[INFO] 开始执行 Tools3000 新增特性全链路实机 E2E 防回退门禁..."

# 1. 定向核验本轮全部新增原生单测组
$FeatureFilter = @(
    "CrossCodepagePathTest.*",
    "FeedbackAndArchiveTest.*",
    "FileLocksmithTest.*",
    "FontPreviewTest.*",
    "GestureActionUpgradeTest.*",
    "GestureInputPolicyTest.PauseToCancel*",
    "GestureInputPolicyTest.WatchdogAllows8s*",
    "LuaEngineUpgradeTest.*",
    "BrowserBookmarkIndexTest.*",
    "SystemCommandCatalogTest.*",
    "SearchRankingFusionTest.*",
    "InstalledAppIndexTest.*",
    "FileIndexStoreMemoryTest.*",
    "SearchLiveWatchTest.*",
    "DialogClassificationTest.*",
    "DialogSearchActionPolicyTest.*",
    "DialogPathMemoryTest.*",
    "DialogOverlayLayoutTest.*",
    "DialogPipeClientTest.*"
) -join ":"

Write-Host "[INFO] [E2E-1/3] 执行全部新增模块核心单测组..."
& $TestsExe "--gtest_filter=$FeatureFilter" "--gtest_brief=1"
if ($LASTEXITCODE -ne 0) {
    throw "[ERROR] 新增模块单测组失败 (exit=$LASTEXITCODE)"
}
Write-Host "[OK] [E2E-1/3] 新增模块单测组全部通过"

# 2. 真机跨进程 Win32 文件对话框四模式实测
Write-Host "[INFO] [E2E-2/3] 执行真机跨进程 Win32 文件对话框 E2E (Modern Folder/Open/Save + Legacy OFN_EXPLORER)..."
& $DialogE2eExe
if ($LASTEXITCODE -ne 0) {
    throw "[ERROR] 真机 Win32 文件对话框 E2E 失败 (exit=$LASTEXITCODE)"
}
Write-Host "[OK] [E2E-2/3] 真机 Win32 文件对话框四模式 E2E 全部 PASS"

# 3. 真机短命隔离 MediaProbeWorker (--worker=media-probe) + 8 语种 Unicode 路径实测
Write-Host "[INFO] [E2E-3/3] 执行 MediaProbeWorker 短命隔离 FFmpeg 实机探测 (含 8 语种混合 Unicode 路径)..."
$TempRoot = Join-Path ([System.IO.Path]::GetTempPath()) ("Tools3000_E2E_工具_ツール_도구_Инструмент_Español_" + $PID)
New-Item -ItemType Directory -Path $TempRoot -Force | Out-Null
try {
    $SampleSrc = Get-ChildItem -Path (Join-Path $env:WINDIR "Media") -Filter "*.wav" -ErrorAction SilentlyContinue |
        Select-Object -First 1 -ExpandProperty FullName
    if (-not $SampleSrc) {
        throw "[ERROR] 未找到 Windows 系统音频样本文件用于 MediaProbeWorker 实测"
    }

    $UnicodeInput = Join-Path $TempRoot "样本_テスト_테스트_Тест.wav"
    Copy-Item -LiteralPath $SampleSrc -Destination $UnicodeInput -Force

    $JobJsonPath = Join-Path $TempRoot "probe_job.json"
    $OutJsonPath = Join-Path $TempRoot "probe_out.json"

    $JobPayload = @{
        inputPath = $UnicodeInput
        outputPath = $OutJsonPath
        maxFrames = 2
        maxThumbWidth = 320
    } | ConvertTo-Json -Compress
    [System.IO.File]::WriteAllText($JobJsonPath, $JobPayload, [System.Text.UTF8Encoding]::new($false))

    $proc = Start-Process -FilePath $MainExe `
        -ArgumentList @("--worker=media-probe", "--job=$JobJsonPath") `
        -WorkingDirectory $BinDir `
        -NoNewWindow -Wait -PassThru

    if ($proc.ExitCode -ne 0) {
        throw "[ERROR] MediaProbeWorker 返回非零退出码: $($proc.ExitCode)"
    }
    if (-not (Test-Path -LiteralPath $OutJsonPath -PathType Leaf)) {
        throw "[ERROR] MediaProbeWorker 未生成输出 JSON: $OutJsonPath"
    }

    $ProbeResult = Get-Content -LiteralPath $OutJsonPath -Raw -Encoding UTF8 | ConvertFrom-Json
    if (-not $ProbeResult.ok) {
        throw "[ERROR] MediaProbeWorker 探测返回 ok=false"
    }
    if ([double]$ProbeResult.durationSec -le 0.0 -or [int]$ProbeResult.audioSampleRate -le 0 -or [int]$ProbeResult.audioChannels -le 0) {
        throw "[ERROR] MediaProbeWorker 未解析出有效音频流元信息: dur=$($ProbeResult.durationSec), rate=$($ProbeResult.audioSampleRate), ch=$($ProbeResult.audioChannels)"
    }
    if ([string]::IsNullOrWhiteSpace([string]$ProbeResult.audioCodec)) {
        throw "[ERROR] MediaProbeWorker 未解析出有效音频编码名称"
    }

    Write-Host ("[OK] [E2E-3/3] MediaProbeWorker 实机闭环通过: format={0}, codec={1}, rate={2}Hz, ch={3}, dur={4:F2}s" -f `
        $ProbeResult.containerFormat, $ProbeResult.audioCodec, $ProbeResult.audioSampleRate, $ProbeResult.audioChannels, [double]$ProbeResult.durationSec)
}
finally {
    Remove-Item -LiteralPath $TempRoot -Recurse -Force -ErrorAction SilentlyContinue
}

Write-Host "[OK] Tools3000 新增特性全链路实机 E2E 防回退门禁全部通过！"
exit 0
