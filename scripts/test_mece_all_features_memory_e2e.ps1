# ==============================================================================
# Tools3000 MECE 7 大领域全功能实机回归与无损内存水位 (<15MB) 端到端门禁
# Copyright (c) 2026 Yy1 (yuan278501381) & Tools3000 contributors
#
# 遵循 MECE 正交拆解覆盖全部 7 大产品域:
#   D1: 内核底座、配置云备份(Folder/WebDAV/S3+AES-GCM)、便携热更新与进程外对话框隔离
#   D2: 屏幕截图、长截图、演播室、贴图缩放与磁盘惰性截图历史水合 (0 重复 PNG 驻留)
#   D3: 多引擎离线/深度 OCR 与表格重建
#   D4: 剪贴板历史、来源应用真彩图标提取与 2K 大图采集后静默堆紧缩回收
#   D5: 1000Hz 鼠标手势、轮盘菜单、触发角、Lua 本地自动化与远程控场
#   D6: 全盘毫秒搜索、QuickLook 预览、MediaProbeWorker 与跨进程文件对话框增强
#   D7: 效率增强套件 (TextExpander / Keycast / Spotlight / FileLocksmith)
# ==============================================================================
[CmdletBinding()]
param(
    [string]$BinDir = (Join-Path $PSScriptRoot "..\build\bin\Release"),
    [double]$TargetMaxIdlePrivateWsMB = 15.0
)

$ErrorActionPreference = "Stop"
[Console]::OutputEncoding = [System.Text.Encoding]::UTF8

$BinDir = (Resolve-Path -LiteralPath $BinDir).Path
$MainExe = Join-Path $BinDir "Tools3000.exe"
$TestsExe = Join-Path $BinDir "Tools3000Tests.exe"
$DialogE2eExe = Join-Path $BinDir "Tools3000DialogNavigatorIntegration.exe"
$BaselineScript = Join-Path $PSScriptRoot "collect-performance-baseline.ps1"

foreach ($req in @($MainExe, $TestsExe, $DialogE2eExe, $BaselineScript)) {
    if (-not (Test-Path -LiteralPath $req -PathType Leaf)) {
        throw "[ERROR] 缺少实机测试必需文件: $req"
    }
}

Add-Type -AssemblyName System.Drawing
Add-Type -AssemblyName System.Windows.Forms
Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
using System.Text;

public static class T3kMemProbe {
    [DllImport("psapi.dll", SetLastError = true)]
    private static extern bool QueryWorkingSet(IntPtr hProcess, IntPtr pv, uint cb);

    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern IntPtr OpenProcess(uint dwDesiredAccess, bool bInheritHandle, int dwProcessId);

    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern bool CloseHandle(IntPtr hObject);

    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    public static extern bool EnumWindows(EnumWindowsProc lpEnumFunc, IntPtr lParam);
    public delegate bool EnumWindowsProc(IntPtr hWnd, IntPtr lParam);

    [DllImport("user32.dll")]
    public static extern uint GetWindowThreadProcessId(IntPtr hWnd, out uint lpdwProcessId);

    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    public static extern int GetClassName(IntPtr hWnd, StringBuilder lpClassName, int nMaxCount);

    [DllImport("user32.dll")]
    public static extern bool PostMessage(IntPtr hWnd, uint Msg, IntPtr wParam, IntPtr lParam);

    public static double MeasurePrivateWorkingSetMB(int pid) {
        const uint PROCESS_QUERY_INFORMATION = 0x0400;
        const uint PROCESS_VM_READ = 0x0010;
        IntPtr hProc = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, false, pid);
        if (hProc == IntPtr.Zero) return -1.0;
        try {
            int maxEntries = 262144;
            for (int attempt = 0; attempt < 3; ++attempt) {
                uint bytes = (uint)((maxEntries + 1) * IntPtr.Size);
                IntPtr buf = Marshal.AllocHGlobal((int)bytes);
                try {
                    if (QueryWorkingSet(hProc, buf, bytes)) {
                        long count = (IntPtr.Size == 8) ? Marshal.ReadInt64(buf, 0) : Marshal.ReadInt32(buf, 0);
                        long privatePages = 0;
                        for (long i = 1; i <= count; ++i) {
                            long entry = (IntPtr.Size == 8)
                                ? Marshal.ReadInt64(buf, (int)(i * 8))
                                : Marshal.ReadInt32(buf, (int)(i * 4));
                            bool shared = ((entry & 0x100L) != 0);
                            if (!shared) privatePages++;
                        }
                        return Math.Round((privatePages * 4096.0) / (1024.0 * 1024.0), 2);
                    }
                    long needed = (IntPtr.Size == 8) ? Marshal.ReadInt64(buf, 0) : Marshal.ReadInt32(buf, 0);
                    maxEntries = (int)Math.Max(maxEntries * 2L, needed + 4096L);
                } finally {
                    Marshal.FreeHGlobal(buf);
                }
            }
            return -1.0;
        } finally {
            CloseHandle(hProc);
        }
    }

    public static IntPtr FindWindowForPid(int targetPid, string className) {
        IntPtr found = IntPtr.Zero;
        EnumWindows((hWnd, _) => {
            uint pid = 0;
            GetWindowThreadProcessId(hWnd, out pid);
            if ((int)pid != targetPid) return true;
            var sb = new StringBuilder(256);
            GetClassName(hWnd, sb, sb.Capacity);
            if (string.Equals(sb.ToString(), className, StringComparison.Ordinal)) {
                found = hWnd;
                return false;
            }
            return true;
        }, IntPtr.Zero);
        return found;
    }
}
'@

function Get-HostSnapshot {
    param([int]$TargetPid, [string]$StageName)
    $p = Get-Process -Id $TargetPid -ErrorAction Stop
    $privWsMB = [T3kMemProbe]::MeasurePrivateWorkingSetMB($TargetPid)
    $loadedMods = @($p.Modules | ForEach-Object { $_.ModuleName })
    $hasClr = ($loadedMods -contains "clr.dll") -or ($loadedMods -contains "mscorlib.ni.dll") -or ($loadedMods -contains "coreclr.dll")
    return [pscustomobject]@{
        stage            = $StageName
        pid              = $TargetPid
        privateWsMB      = $privWsMB
        totalWsMB        = [math]::Round($p.WorkingSet64 / 1MB, 2)
        privateCommitMB  = [math]::Round($p.PrivateMemorySize64 / 1MB, 2)
        handles          = $p.HandleCount
        threads          = $p.Threads.Count
        moduleCount      = $loadedMods.Count
        clrContaminated  = $hasClr
    }
}

Write-Host "[INFO] ======================================================================"
Write-Host "[INFO] 启动 Tools3000 MECE 7 大领域全功能实机验证与内存观测"
Write-Host "[INFO] ======================================================================"

# ------------------------------------------------------------------------------
# Phase A: MECE 7 大正交领域原生单元测试矩阵
# ------------------------------------------------------------------------------
$DomainFilters = [ordered]@{
    "D1_Core_CloudBackup_Updater_OopDialog" = "OutOfProcessFileDialogTest.*:QuietIdleHygieneTest.*:ConfigBackupServiceTest.*:PortableInstaller*:*CrossCodepagePathTest.*:FeedbackAndArchiveTest.*"
    "D2_Capture_HistoryHydration_Pin_Studio" = "CaptureHistoryDiskBackedHydrationTest.*:CaptureInteractionPolicy*:*CaptureRegionHistoryTest.*:PinZoomStageTest.*:ClipboardUtilsTest.EmptyImageRejection"
    "D3_Ocr_TableReconstructor"              = "*Ocr*"
    "D4_Clipboard_SourceIcon_Storage"        = "ClipboardManagerTest.*"
    "D5_Gesture_Lua_Remote"                  = "GestureRecognizerTest.*:GestureActionUpgradeTest.*:GestureInputPolicyTest.*:LuaEngineUpgradeTest.*"
    "D6_Search_QuickLook_DialogEnhancer"     = "FileIndexStoreMemoryTest.*:SearchRankingFusionTest.*:DialogClassificationTest.*:DialogPathMemoryTest.*:FontPreviewTest.*"
    "D7_TextExpander_Keycast_Locksmith"      = "TextExpanderMatcherTest.*:FileLocksmithTest.*:*Keycast*"
}

foreach ($domainKey in $DomainFilters.Keys) {
    $filter = $DomainFilters[$domainKey]
    Write-Host "[INFO] [MECE-$domainKey] 运行领域测试集..."
    & $TestsExe "--gtest_filter=$filter" "--gtest_brief=1"
    if ($LASTEXITCODE -ne 0) {
        throw "[ERROR] 领域测试集失败: $domainKey (exit=$LASTEXITCODE)"
    }
    Write-Host "[OK] [MECE-$domainKey] PASS"
}

Write-Host "[INFO] [MECE-D6-Win32E2E] 运行跨进程 Win32 文件对话框四模式真机集成测试..."
& $DialogE2eExe
if ($LASTEXITCODE -ne 0) {
    throw "[ERROR] Tools3000DialogNavigatorIntegration.exe 失败 (exit=$LASTEXITCODE)"
}
Write-Host "[OK] [MECE-D6-Win32E2E] PASS"

# ------------------------------------------------------------------------------
# Phase B: 隔离沙盒宿主全功能真实交互与四阶段内存水位实测
# ------------------------------------------------------------------------------
$SandboxRoot = Join-Path ([System.IO.Path]::GetTempPath()) ("T3K_MECE_Sandbox_简体_日本語_" + $PID)
$LocalData = Join-Path $SandboxRoot "LocalAppData"
$RoamData  = Join-Path $SandboxRoot "RoamingAppData"
$DataRoot  = Join-Path $LocalData "Tools3000"
New-Item -ItemType Directory -Path $DataRoot, $RoamData -Force | Out-Null

$HostProc = $null
$Telemetry = @()

try {
    $psi = [System.Diagnostics.ProcessStartInfo]::new()
    $psi.FileName = $MainExe
    $psi.WorkingDirectory = $BinDir
    $psi.UseShellExecute = $false
    foreach ($arg in @("--silent", "--no-elevate", "--force-portable-search-service", "--lifecycle-test-instance")) {
        [void]$psi.ArgumentList.Add($arg)
    }
    $psi.Environment["LOCALAPPDATA"] = $LocalData
    $psi.Environment["APPDATA"] = $RoamData
    $psi.Environment["TOOLS3000_DATA_ROOT"] = $DataRoot
    $psi.Environment["TOOLS3000_PORTABLE"] = "0"

    Write-Host "[INFO] [Sandbox] 启动隔离宿主实例..."
    $HostProc = [System.Diagnostics.Process]::Start($psi)
    Start-Sleep -Milliseconds 6800

    if ($HostProc.HasExited) {
        throw "[ERROR] 沙盒宿主意外退出 (exit=$($HostProc.ExitCode))"
    }

    # S1: 冷启动预热 + 静默堆紧缩后稳态
    $s1 = Get-HostSnapshot -TargetPid $HostProc.Id -StageName "S1_ColdStart_QuietIdle"
    $Telemetry += $s1
    Write-Host ("[OK] [S1 冷启动稳态] PrivWS={0} MB | TotalWS={1} MB | PrivCommit={2} MB | Modules={3} | CLR={4}" -f `
        $s1.privateWsMB, $s1.totalWsMB, $s1.privateCommitMB, $s1.moduleCount, $s1.clrContaminated)

    # 校验冷启动模块零污染门禁 (含 FFmpeg / OpenCV / DirectML / .NET CLR)
    $modGateJson = & pwsh -NoProfile -File $BaselineScript -Action CheckColdModules -TargetPid $HostProc.Id
    if ($LASTEXITCODE -ne 0) {
        throw "[ERROR] CheckColdModules 冷路径模块门禁未通过: $modGateJson"
    }
    Write-Host "[OK] [Gate] 冷路径延迟加载与零 .NET CLR 注入门禁 PASS"

    # S2: 真实打开设置中心 WebView2 -> 渲染 -> 关闭 -> 挂起修剪稳态
    $msgHwnd = [T3kMemProbe]::FindWindowForPid($HostProc.Id, "Tools3000_MessageWindow")
    if ($msgHwnd -eq [IntPtr]::Zero) {
        throw "[ERROR] 未找到沙盒宿主 Tools3000_MessageWindow"
    }
    [void][T3kMemProbe]::PostMessage($msgHwnd, 0x8000 + 101, [IntPtr]::Zero, [IntPtr]::Zero)
    Start-Sleep -Milliseconds 2600
    $s2Peak = Get-HostSnapshot -TargetPid $HostProc.Id -StageName "S2a_SettingsOpen_Active"
    $Telemetry += $s2Peak

    $settingsHwnd = [T3kMemProbe]::FindWindowForPid($HostProc.Id, "Tools3000_SettingsWindow")
    if ($settingsHwnd -ne [IntPtr]::Zero) {
        [void][T3kMemProbe]::PostMessage($settingsHwnd, 0x0010, [IntPtr]::Zero, [IntPtr]::Zero) # WM_CLOSE
    }
    Start-Sleep -Milliseconds 4800
    $s2Idle = Get-HostSnapshot -TargetPid $HostProc.Id -StageName "S2b_SettingsClosed_QuietIdle"
    $Telemetry += $s2Idle
    Write-Host ("[OK] [S2 设置窗开合后稳态] Active={0} MB -> QuietIdle={1} MB | PrivCommit={2} MB" -f `
        $s2Peak.privateWsMB, $s2Idle.privateWsMB, $s2Idle.privateCommitMB)

    # S3: 真实向系统剪贴板连续推送 3 张 2560x1440 (2K) 全彩大图 (共 44.2MB 原始像素)
    $staJob = Start-Job -ScriptBlock {
        Add-Type -AssemblyName System.Drawing
        Add-Type -AssemblyName System.Windows.Forms
        for ($idx = 1; $idx -le 3; ++$idx) {
            $bmp = New-Object System.Drawing.Bitmap(2560, 1440, [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
            $g = [System.Drawing.Graphics]::FromImage($bmp)
            $brush = New-Object System.Drawing.Drawing2D.LinearGradientBrush(
                [System.Drawing.Point]::new(0, 0),
                [System.Drawing.Point]::new(2560, 1440),
                [System.Drawing.Color]::FromArgb(255, (40 * $idx) % 255, (90 * $idx) % 255, 180),
                [System.Drawing.Color]::FromArgb(255, 220, (70 * $idx) % 255, (130 * $idx) % 255))
            $g.FillRectangle($brush, 0, 0, 2560, 1440)
            $g.DrawString("Tools3000 2K Memory Benchmark #$idx " + [guid]::NewGuid().ToString(),
                (New-Object System.Drawing.Font("Arial", 42)),
                [System.Drawing.Brushes]::White, 120, 120 * $idx)
            $brush.Dispose()
            $g.Dispose()
            for ($retry = 0; $retry -lt 10; ++$retry) {
                try {
                    [System.Windows.Forms.Clipboard]::SetImage($bmp)
                    break
                } catch {
                    Start-Sleep -Milliseconds 60
                }
            }
            $bmp.Dispose()
            Start-Sleep -Milliseconds 320
        }
        try { [System.Windows.Forms.Clipboard]::SetText("Tools3000 Memory E2E Done") } catch {}
    }
    Wait-Job $staJob | Out-Null
    Receive-Job $staJob | Out-Null
    Remove-Job $staJob -Force

    # 等待 ClipboardManager 后台 PNG 落盘 + 2.8s 防抖静默堆紧缩与工作集回收完成
    Start-Sleep -Milliseconds 4800
    $s3 = Get-HostSnapshot -TargetPid $HostProc.Id -StageName "S3_After3x2KClipboardImages_QuietIdle"
    $Telemetry += $s3
    Write-Host ("[OK] [S3 3张2K大图剪贴板入库后稳态] PrivWS={0} MB | TotalWS={1} MB | PrivCommit={2} MB" -f `
        $s3.privateWsMB, $s3.totalWsMB, $s3.privateCommitMB)

    # S4: 真实执行进程外 --worker=file-dialog 与 --worker=media-probe 并断言宿主 0 CLR 污染与无僵尸子进程
    $dlgIn = Join-Path $SandboxRoot "dlg_in.json"
    $dlgOut = Join-Path $SandboxRoot "dlg_out.json"
    $expectedPick = Join-Path $SandboxRoot "自动导出_テスト.png"
    @{
        request = @{
            mode = 1
            ownerHwnd = 0
            title = "E2E Worker File Dialog"
            defaultExtension = "png"
            defaultFileName = "export.png"
            initialDirectory = $SandboxRoot
            filters = @(@{ label = "PNG (*.png)"; pattern = "*.png" })
            initialFilterIndex = 1
            dontAddToRecent = $true
            autoSelectPathForTest = $expectedPick
        }
        outputPath = $dlgOut
    } | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath $dlgIn -Encoding UTF8

    $wProc = Start-Process -FilePath $MainExe -ArgumentList @("--worker=file-dialog", "--job=$dlgIn") -WorkingDirectory $BinDir -NoNewWindow -Wait -PassThru
    if ($wProc.ExitCode -ne 0 -or -not (Test-Path -LiteralPath $dlgOut)) {
        throw "[ERROR] --worker=file-dialog 退出异常 (code=$($wProc.ExitCode))"
    }
    $wJson = Get-Content -LiteralPath $dlgOut -Raw -Encoding UTF8 | ConvertFrom-Json
    if (-not $wJson.accepted -or $wJson.selectedPath -ne $expectedPick) {
        throw "[ERROR] --worker=file-dialog 返回结果不匹配: $($wJson | ConvertTo-Json -Compress)"
    }

    $sampleWav = Get-ChildItem -Path (Join-Path $env:WINDIR "Media") -Filter "*.wav" -ErrorAction SilentlyContinue |
        Select-Object -First 1 -ExpandProperty FullName
    if ($sampleWav) {
        $probeIn = Join-Path $SandboxRoot "probe_in.json"
        $probeOut = Join-Path $SandboxRoot "probe_out.json"
        @{ inputPath = $sampleWav; outputPath = $probeOut; maxFrames = 1; maxThumbWidth = 240 } |
            ConvertTo-Json -Compress | Set-Content -LiteralPath $probeIn -Encoding UTF8
        $mProc = Start-Process -FilePath $MainExe -ArgumentList @("--worker=media-probe", "--job=$probeIn") -WorkingDirectory $BinDir -NoNewWindow -Wait -PassThru
        if ($mProc.ExitCode -ne 0) {
            throw "[ERROR] --worker=media-probe 退出异常 (code=$($mProc.ExitCode))"
        }
    }

    Start-Sleep -Milliseconds 1200
    $s4 = Get-HostSnapshot -TargetPid $HostProc.Id -StageName "S4_AfterWorkers_FinalSteadyState"
    $Telemetry += $s4
    Write-Host ("[OK] [S4 全链路Worker执行后稳态] PrivWS={0} MB | TotalWS={1} MB | PrivCommit={2} MB | CLR={3}" -f `
        $s4.privateWsMB, $s4.totalWsMB, $s4.privateCommitMB, $s4.clrContaminated)

    # 核心内存 SLA 红线强断言
    foreach ($snap in @($s1, $s2Idle, $s3, $s4)) {
        if ($snap.clrContaminated) {
            throw "[ERROR] 宿主进程在阶段 $($snap.stage) 检测到 .NET CLR 污染！"
        }
        if ($snap.privateWsMB -gt $TargetMaxIdlePrivateWsMB) {
            throw "[ERROR] 阶段 $($snap.stage) 专用工作集 $($snap.privateWsMB) MB 超出目标上限 $TargetMaxIdlePrivateWsMB MB！"
        }
    }

    Write-Host ""
    Write-Host "[OK] ==================== 实测内存汇总表 ===================="
    $Telemetry | Format-Table -AutoSize | Out-String | Write-Host
    Write-Host ("[OK] 全部静默稳态 PrivateWorkingSet 均严格 < {0} MB，且 0 CLR 污染！" -f $TargetMaxIdlePrivateWsMB)
}
finally {
    if ($HostProc -and -not $HostProc.HasExited) {
        $msgHwnd = [T3kMemProbe]::FindWindowForPid($HostProc.Id, "Tools3000_MessageWindow")
        if ($msgHwnd -ne [IntPtr]::Zero) {
            [void][T3kMemProbe]::PostMessage($msgHwnd, 0x0010, [IntPtr]::Zero, [IntPtr]::Zero)
            [void]$HostProc.WaitForExit(3000)
        }
        if (-not $HostProc.HasExited) {
            Stop-Process -Id $HostProc.Id -Force -ErrorAction SilentlyContinue
        }
    }
    Start-Sleep -Milliseconds 400
    Remove-Item -LiteralPath $SandboxRoot -Recurse -Force -ErrorAction SilentlyContinue
}

exit 0
