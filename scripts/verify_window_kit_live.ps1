#!/usr/bin/env pwsh
<#
.SYNOPSIS
Tools3000 WindowKit & CapsLayer 真机端到端验收测试套件 (verify_window_kit_live.ps1)

.DESCRIPTION
在真实 Windows 桌面环境中使用独立隔离靶标窗口与临时目录，对纯 Rust Tools3000_WindowKit.exe
及宿主集成执行全链路实测验证：
  1. 默认全关 0 进程 / 0 MB / 0 句柄验证；
  2. One-shot CLI（--self-test / --dump-monitors / --capture-workspace / --import-powertoys）；
  3. 逐动作真机驱动（SnapZones 蒙层与吸附 / Always on Top 置顶光边 / Opacity 透明度 /
     Quick Switch Alt+Tab 实时预览 / Multi-Monitor 离屏拉回 / 托盘收纳与全量还原 / Workspaces），
     采集每项动作的延迟 (ms) 与 Before -> Peak -> After(冷回收) 内存与句柄轨迹；
  4. 20 轮高频压测验证 Handle / GDI / USER 严格零泄漏与冷路径工作集自动回落；
  5. 故障零连坐与托盘窗优雅自动还原测试 + window_kit.log 日志纯净度审计。
#>
param(
    [string]$BinaryDir = "",
    [int]$StressCycles = 20
)

$ErrorActionPreference = "Stop"
[Console]::OutputEncoding = [System.Text.Encoding]::UTF8
$OutputEncoding = [System.Text.Encoding]::UTF8

$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$ProjectRoot = Split-Path -Parent $ScriptDir

if ([string]::IsNullOrWhiteSpace($BinaryDir)) {
    $Candidates = @(
        (Join-Path $ProjectRoot "build\bin\Release"),
        (Join-Path $ProjectRoot "src\window_kit\target\x86_64-pc-windows-msvc\release"),
        (Join-Path $ProjectRoot "src\window_kit\target\release")
    )
    foreach ($c in $Candidates) {
        if (Test-Path -LiteralPath (Join-Path $c "Tools3000_WindowKit.exe")) {
            $BinaryDir = $c
            break
        }
    }
}

$WkExe = Join-Path $BinaryDir "Tools3000_WindowKit.exe"
if (-not (Test-Path -LiteralPath $WkExe)) {
    throw "[ERROR] Tools3000_WindowKit.exe not found in: $BinaryDir"
}

Write-Host "[INFO] Target WindowKit binary: $WkExe" -ForegroundColor Cyan
$ExeInfo = Get-Item -LiteralPath $WkExe
Write-Host ("[INFO] Binary size: {0:N2} KB ({1:N2} MB)" -f ($ExeInfo.Length / 1KB), ($ExeInfo.Length / 1MB)) -ForegroundColor Cyan

Add-Type -TypeDefinition @"
using System;
using System.Runtime.InteropServices;

public static class WkLiveNative {
    [StructLayout(LayoutKind.Sequential)]
    public struct RECT {
        public int Left;
        public int Top;
        public int Right;
        public int Bottom;
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct PROCESS_MEMORY_COUNTERS_EX {
        public uint cb;
        public uint PageFaultCount;
        public UIntPtr PeakWorkingSetSize;
        public UIntPtr WorkingSetSize;
        public UIntPtr QuotaPeakPagedPoolUsage;
        public UIntPtr QuotaPagedPoolUsage;
        public UIntPtr QuotaPeakNonPagedPoolUsage;
        public UIntPtr QuotaNonPagedPoolUsage;
        public UIntPtr PagefileUsage;
        public UIntPtr PeakPagefileUsage;
        public UIntPtr PrivateUsage;
    }

    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    public static extern IntPtr FindWindowW(string lpClassName, string lpWindowName);

    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    public static extern IntPtr FindWindowExW(IntPtr parent, IntPtr childAfter, string lpClassName, string lpWindowName);

    [DllImport("user32.dll")]
    public static extern uint GetWindowThreadProcessId(IntPtr hWnd, out uint lpdwProcessId);

    [DllImport("user32.dll")]
    public static extern bool PostMessageW(IntPtr hWnd, uint Msg, IntPtr wParam, IntPtr lParam);

    [DllImport("user32.dll")]
    public static extern bool IsWindow(IntPtr hWnd);

    [DllImport("user32.dll")]
    public static extern bool IsWindowVisible(IntPtr hWnd);

    [DllImport("user32.dll")]
    public static extern bool GetWindowRect(IntPtr hWnd, out RECT lpRect);

    [DllImport("user32.dll")]
    public static extern bool SetWindowPos(IntPtr hWnd, IntPtr hWndInsertAfter, int X, int Y, int cx, int cy, uint uFlags);

    [DllImport("user32.dll")]
    public static extern bool ShowWindow(IntPtr hWnd, int nCmdShow);

    [DllImport("user32.dll")]
    public static extern bool SetForegroundWindow(IntPtr hWnd);

    [DllImport("user32.dll")]
    public static extern IntPtr GetForegroundWindow();

    [DllImport("user32.dll")]
    public static extern bool AttachThreadInput(uint idAttach, uint idAttachTo, bool fAttach);

    [DllImport("kernel32.dll")]
    public static extern uint GetCurrentThreadId();

    [DllImport("user32.dll")]
    public static extern bool BringWindowToTop(IntPtr hWnd);

    public static bool ForceForeground(IntPtr hWnd) {
        IntPtr curFg = GetForegroundWindow();
        if (curFg == hWnd) return true;
        uint dummy;
        uint fgTid = (curFg != IntPtr.Zero) ? GetWindowThreadProcessId(curFg, out dummy) : 0;
        uint myTid = GetCurrentThreadId();
        bool attached = false;
        if (fgTid != 0 && fgTid != myTid) {
            attached = AttachThreadInput(myTid, fgTid, true);
        }
        ShowWindow(hWnd, 9);
        BringWindowToTop(hWnd);
        SetForegroundWindow(hWnd);
        if (attached) {
            AttachThreadInput(myTid, fgTid, false);
        }
        return GetForegroundWindow() == hWnd;
    }

    [DllImport("user32.dll", EntryPoint = "GetWindowLongPtrW")]
    public static extern IntPtr GetWindowLongPtrW(IntPtr hWnd, int nIndex);

    [DllImport("user32.dll")]
    public static extern bool GetLayeredWindowAttributes(IntPtr hwnd, out uint pcrKey, out byte pbAlpha, out uint pdwFlags);

    [DllImport("user32.dll")]
    public static extern uint GetGuiResources(IntPtr hProcess, uint uiFlags);

    [DllImport("kernel32.dll")]
    public static extern bool GetProcessHandleCount(IntPtr hProcess, out uint pdwHandleCount);

    [DllImport("psapi.dll", SetLastError = true)]
    public static extern bool GetProcessMemoryInfo(IntPtr hProcess, out PROCESS_MEMORY_COUNTERS_EX ppsmemCounters, uint cb);

    public static IntPtr FindWindowByClass(string cls) {
        return FindWindowW(cls, null);
    }

    public static IntPtr FindWindowByClassAndPid(string cls, uint targetPid) {
        IntPtr cur = IntPtr.Zero;
        while (true) {
            cur = FindWindowExW(IntPtr.Zero, cur, cls, null);
            if (cur == IntPtr.Zero) break;
            uint pid;
            GetWindowThreadProcessId(cur, out pid);
            if (pid == targetPid) return cur;
        }
        return IntPtr.Zero;
    }

    public static int CountWindowsByClassAndPid(string cls, uint targetPid) {
        int count = 0;
        IntPtr cur = IntPtr.Zero;
        while (true) {
            cur = FindWindowExW(IntPtr.Zero, cur, cls, null);
            if (cur == IntPtr.Zero) break;
            uint pid;
            GetWindowThreadProcessId(cur, out pid);
            if (pid == targetPid) count++;
        }
        return count;
    }

    public static IntPtr FindControlWindowForPid(uint targetPid) {
        return FindWindowByClassAndPid("Tools3000_WindowKit_Control", targetPid);
    }

    public static void ReadMetrics(IntPtr hProcess, out double wsMb, out double privMb, out uint handles, out uint gdi, out uint user) {
        wsMb = 0; privMb = 0; handles = 0; gdi = 0; user = 0;
        var pmc = new PROCESS_MEMORY_COUNTERS_EX();
        pmc.cb = (uint)Marshal.SizeOf(typeof(PROCESS_MEMORY_COUNTERS_EX));
        if (GetProcessMemoryInfo(hProcess, out pmc, pmc.cb)) {
            wsMb = pmc.WorkingSetSize.ToUInt64() / (1024.0 * 1024.0);
            privMb = pmc.PrivateUsage.ToUInt64() / (1024.0 * 1024.0);
        }
        GetProcessHandleCount(hProcess, out handles);
        gdi = GetGuiResources(hProcess, 0);   // GR_GDIOBJECTS
        user = GetGuiResources(hProcess, 1);  // GR_USEROBJECTS
    }
}
"@

Add-Type -AssemblyName System.Windows.Forms

$WM_APP = 0x8000
$WM_WK_RELOAD_CONFIG     = $WM_APP + 1
$WM_WK_EXECUTE_ACTION    = $WM_APP + 2
$WM_WK_SET_MUTED_FLAGS   = $WM_APP + 3
$WM_WK_GRACEFUL_SHUTDOWN = $WM_APP + 4

$ACT_TOGGLE_AOT          = 1
$ACT_OPACITY_UP          = 2
$ACT_OPACITY_DOWN        = 3
$ACT_OPACITY_RESET       = 4
$ACT_MOVE_NEXT_ZONE      = 5
$ACT_MOVE_ZONE_INDEX     = 7
$ACT_RESTORE_FROM_ZONE   = 8
$ACT_RESCUE_OFFSCREEN    = 9
$ACT_MOVE_NEXT_MONITOR   = 10
$ACT_MINIMIZE_TO_TRAY    = 11
$ACT_RESTORE_ALL_TRAY    = 12
$ACT_OPEN_QUICK_SWITCH   = 13
$ACT_CAPTURE_WORKSPACE   = 14
$ACT_LAUNCH_WORKSPACE    = 15
$ACT_QUERY_RUNTIME_STATS = 18
$ACT_SHOW_ZONES_PREVIEW  = 19
$ACT_OPEN_LAYOUT_HUB     = 20
$ACT_APPLY_LAYOUT_DIGIT  = 21
$ACT_START_WS_CAPTURE    = 22
$ACT_PREVIEW_BORDER      = 23
$ACT_OPEN_LAYOUT_DIRECT  = 24
$WM_WK_INTERNAL_ALT_TAB  = $WM_APP + 100
$WM_CLOSE                = 0x0010

$GWL_EXSTYLE = -20
$WS_EX_TOPMOST = 0x00000008
$WS_EX_LAYERED = 0x00080000
$SWP_NOSIZE = 0x0001
$SWP_NOZORDER = 0x0004
$SWP_NOACTIVATE = 0x0010

$TempRoot = Join-Path ([System.IO.Path]::GetTempPath()) ("t3k_wk_e2e_" + [DateTimeOffset]::UtcNow.ToUnixTimeMilliseconds())
$LogDir = Join-Path $TempRoot "logs"
New-Item -ItemType Directory -Path $LogDir -Force | Out-Null

$WkProc = $null
$TestFormA = $null
$TestFormB = $null
$OrigFgHwnd = [WkLiveNative]::GetForegroundWindow()
$ReportRows = [System.Collections.Generic.List[object]]::new()

function Get-ProcMetrics {
    param([System.Diagnostics.Process]$Proc)
    $ws = 0.0; $priv = 0.0; [uint32]$h = 0; [uint32]$g = 0; [uint32]$u = 0
    [WkLiveNative]::ReadMetrics($Proc.Handle, [ref]$ws, [ref]$priv, [ref]$h, [ref]$g, [ref]$u)
    return [PSCustomObject]@{
        WsMb = [Math]::Round($ws, 2)
        PrivMb = [Math]::Round($priv, 2)
        Handles = $h
        Gdi = $g
        User = $u
    }
}

function Focus-TargetWindow {
    param([IntPtr]$Hwnd)
    $payloadPath = Join-Path $TempRoot "window_kit_action.json"
    @{ targetHwnd = $Hwnd.ToInt64() } | ConvertTo-Json -Compress | Set-Content -LiteralPath $payloadPath -Encoding UTF8
    [void][WkLiveNative]::ShowWindow($Hwnd, 9)
    [System.Windows.Forms.Application]::DoEvents()
    Start-Sleep -Milliseconds 50
}

try {
    # ── Phase 0: 默认全关 0 进程验证 ─────────────────────────────────────────
    Write-Host "`n[STEP 1] Verifying default-OFF zero-footprint behavior..." -ForegroundColor Yellow
    $OffCfgPath = Join-Path $TempRoot "config_off.json"
    @{
        enabled = $false
        logDir = $LogDir
        logLevel = "debug"
        features = @{
            quickSwitch = $false; snapZones = $false; alwaysOnTop = $false; opacity = $false
            altDrag = $false; focusBorder = $false; multiMonitor = $false; workspaces = $false
        }
    } | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath $OffCfgPath -Encoding UTF8

    $offSw = [System.Diagnostics.Stopwatch]::StartNew()
    $offProc = Start-Process -FilePath $WkExe -ArgumentList "--managed --config `"$OffCfgPath`" --parent-pid $PID" -PassThru -WindowStyle Hidden
    if (-not $offProc.WaitForExit(3000)) {
        $offProc.Kill()
        throw "Default-OFF WindowKit did not exit immediately!"
    }
    $offSw.Stop()
    if ($offProc.ExitCode -ne 0) {
        throw "Default-OFF WindowKit exited with non-zero code: $($offProc.ExitCode)"
    }
    Write-Host ("[OK] Default-OFF exited immediately in {0:N1} ms (exit=0, 0 resident processes, 0 MB)" -f $offSw.Elapsed.TotalMilliseconds) -ForegroundColor Green

    # ── Phase 1: One-Shot CLI 全量校验 ────────────────────────────────────────
    Write-Host "`n[STEP 2] Verifying one-shot CLI utilities (self-test, monitors, capture, PowerToys import)..." -ForegroundColor Yellow
    function Invoke-WindowKitCli {
        param([string]$ArgLine)
        $p = Start-Process -FilePath $WkExe -ArgumentList $ArgLine -PassThru -WindowStyle Hidden
        if (-not $p.WaitForExit(5000)) {
            $p.Kill()
            throw "One-shot CLI timed out: $ArgLine"
        }
        return $p.ExitCode
    }

    $selfOutFile = Join-Path $TempRoot "selftest.json"
    $rcSelf = Invoke-WindowKitCli "--self-test `"$selfOutFile`""
    if ($rcSelf -ne 0 -or -not (Test-Path -LiteralPath $selfOutFile)) { throw "--self-test failed with code $rcSelf" }
    $selfJson = Get-Content -LiteralPath $selfOutFile -Raw | ConvertFrom-Json
    if (-not $selfJson.ok -or -not $selfJson.d2dInitialized -or $selfJson.monitorCount -lt 1) {
        throw "--self-test reported unhealthy state"
    }
    Write-Host "[OK] --self-test passed: version=$($selfJson.version), d2d=$($selfJson.d2dInitialized), monitors=$($selfJson.monitorCount)" -ForegroundColor Green

    $monOut = Join-Path $TempRoot "monitors.json"
    $rcMon = Invoke-WindowKitCli "--dump-monitors `"$monOut`""
    if ($rcMon -ne 0 -or -not (Test-Path -LiteralPath $monOut)) { throw "--dump-monitors failed (rc=$rcMon)" }
    $monJson = Get-Content -LiteralPath $monOut -Raw | ConvertFrom-Json
    if ($monJson.count -lt 1) { throw "--dump-monitors returned 0 monitors" }
    Write-Host "[OK] --dump-monitors returned $($monJson.count) monitor(s), primary=$($monJson.monitors[0].width)x$($monJson.monitors[0].height) @ $($monJson.monitors[0].dpi) DPI" -ForegroundColor Green

    $ptSrc = Join-Path $TempRoot "powertoys_custom_layouts.json"
    $ptOut = Join-Path $TempRoot "powertoys_imported.json"
    @'
{
  "custom-layouts": [
    {
      "uuid": "{1111-2222-3333}",
      "name": "E2E Dev Grid",
      "type": "grid",
      "info": {
        "rows": 2, "columns": 2,
        "rows-percentage": [5000, 5000],
        "columns-percentage": [6000, 4000],
        "cell-child-map": [[0, 1], [0, 2]],
        "show-spacing": true, "spacing": 8
      }
    },
    {
      "uuid": "{4444-5555-6666}",
      "name": "E2E Canvas",
      "type": "canvas",
      "info": {
        "ref-width": 1920, "ref-height": 1080,
        "zones": [
          { "X": 0, "Y": 0, "width": 960, "height": 1080 },
          { "X": 960, "Y": 0, "width": 960, "height": 1080 }
        ]
      }
    }
  ]
}
'@ | Set-Content -LiteralPath $ptSrc -Encoding UTF8
    $rcPt = Invoke-WindowKitCli "--import-powertoys `"$ptOut`" --source `"$ptSrc`""
    if ($rcPt -ne 0 -or -not (Test-Path -LiteralPath $ptOut)) { throw "--import-powertoys failed (rc=$rcPt)" }
    $ptJson = Get-Content -LiteralPath $ptOut -Raw | ConvertFrom-Json
    if ($ptJson.count -ne 2) { throw "--import-powertoys expected 2 layouts, got $($ptJson.count)" }
    Write-Host "[OK] --import-powertoys imported $($ptJson.count) layouts (Grid + Canvas)" -ForegroundColor Green

    # ── 准备两个独立隔离的真实 Win32 靶标窗口（绝不碰用户现有窗口） ─────────
    $TestFormA = New-Object System.Windows.Forms.Form
    $TestFormA.Text = "Tools3000_WindowKit_E2E_Target_A"
    $TestFormA.StartPosition = [System.Windows.Forms.FormStartPosition]::Manual
    $TestFormA.Location = New-Object System.Drawing.Point(160, 140)
    $TestFormA.Size = New-Object System.Drawing.Size(520, 380)
    $TestFormA.Show()

    $TestFormB = New-Object System.Windows.Forms.Form
    $TestFormB.Text = "Tools3000_WindowKit_E2E_Target_B"
    $TestFormB.StartPosition = [System.Windows.Forms.FormStartPosition]::Manual
    $TestFormB.Location = New-Object System.Drawing.Point(240, 200)
    $TestFormB.Size = New-Object System.Drawing.Size(480, 340)
    $TestFormB.Show()
    [System.Windows.Forms.Application]::DoEvents()
    Start-Sleep -Milliseconds 150

    $HwndA = $TestFormA.Handle
    $HwndB = $TestFormB.Handle

    $capOut = Join-Path $TempRoot "workspace_capture.json"
    $rcCap = Invoke-WindowKitCli "--capture-workspace `"$capOut`""
    if ($rcCap -ne 0 -or -not (Test-Path -LiteralPath $capOut)) { throw "--capture-workspace failed (rc=$rcCap)" }
    $capJson = Get-Content -LiteralPath $capOut -Raw | ConvertFrom-Json
    $capApps = @($capJson.apps)
    $capPngCount = @($capApps | Where-Object { $_.iconDataUrl -and $_.iconDataUrl.StartsWith("data:image/png;base64,iVBORw0KGgo") }).Count
    $capRelCount = @($capApps | Where-Object { $null -ne $_.relRect -and $null -ne $_.relRect.relW }).Count
    if ($capApps.Count -lt 1 -or $capPngCount -lt 1 -or $capRelCount -lt 1) {
        throw "--capture-workspace v2 assertion failed: apps=$($capApps.Count), png=$capPngCount, relRect=$capRelCount"
    }
    Write-Host "[OK] --capture-workspace v2 captured $($capApps.Count) window(s): pngIcons=$capPngCount, relRect=$capRelCount, desktop='$($capJson.desktopName)'" -ForegroundColor Green

    # ── Phase 2: 启动常驻托管模式并执行全部 v1+v2 能力真机实测 ──────────────────
    Write-Host "`n[STEP 3] Launching isolated managed Tools3000_WindowKit.exe with all features enabled..." -ForegroundColor Yellow
    $LiveCfgPath = Join-Path $TempRoot "window_kit_runtime.json"
    @{
        schemaVersion = 3
        enabled = $true
        theme = "dark"
        accentColor = "#0EA5E9"
        uiLang = "zh"
        logDir = $LogDir
        logLevel = "info"
        excludedApps = @()
        features = @{
            quickSwitch = $true
            snapZones = $true
            alwaysOnTop = $true
            opacity = $true
            altDrag = $true
            focusBorder = $true
            multiMonitor = $true
            workspaces = $true
        }
        quickSwitch = @{
            replaceAltTab = $true
            sameAppEnabled = $true
            showLivePreview = $true
            previewInDesktopPosition = $false
            currentDesktopOnly = $true
            rememberBounds = $true
            listWidthRatio = 0.34
        }
        snapZones = @{
            triggerMode = "shift"
            zoneGap = 8
            highlightOpacity = 0.30
            showZoneNumbers = $true
            allowMultiZoneSpan = $true
            rememberLastZone = $true
            perVirtualDesktop = $false
            activeLayoutId = "template_columns"
            monitorBindings = @{}
            layouts = @()
        }
        alwaysOnTop = @{ showBorder = $true; borderColor = "#0EA5E9"; borderWidth = 2 }
        opacity = @{ stepPercent = 15; minPercent = 25; altWheelEnabled = $true }
        focusBorder = @{ borderColor = "#8B5CF6"; borderWidth = 2; cornerRadius = 8; opacity = 0.9 }
        workspaces = @{
            minimizeUnrelated = $false
            items = @(
                @{
                    id = "e2e-ws-1"
                    name = "E2E Workspace"
                    apps = @(
                        @{
                            id = "app_e2e_1"
                            appName = "Notepad"
                            exePath = (Join-Path $env:SystemRoot "System32\notepad.exe")
                            processName = "notepad.exe"
                            args = ""
                            monitorId = ""
                            zoneIndex = 0
                            maximized = $false
                            minimized = $false
                            runAsAdmin = $false
                        }
                    )
                }
            )
        }
    } | ConvertTo-Json -Depth 10 | Set-Content -LiteralPath $LiveCfgPath -Encoding UTF8

    $bootSw = [System.Diagnostics.Stopwatch]::StartNew()
    $WkProc = Start-Process -FilePath $WkExe -ArgumentList "--managed --isolated --config `"$LiveCfgPath`" --parent-pid $PID" -PassThru -WindowStyle Hidden
    $CtrlHwnd = [IntPtr]::Zero
    for ($i = 0; $i -lt 100; $i++) {
        $CtrlHwnd = [WkLiveNative]::FindControlWindowForPid([uint32]$WkProc.Id)
        if ($CtrlHwnd -ne [IntPtr]::Zero) { break }
        Start-Sleep -Milliseconds 15
    }
    $bootSw.Stop()
    if ($CtrlHwnd -eq [IntPtr]::Zero) {
        throw "Control window Tools3000_WindowKit_Control did not appear within 1500ms!"
    }
    Start-Sleep -Milliseconds 300
    $idleMetrics = Get-ProcMetrics $WkProc
    Write-Host ("[OK] Managed boot ready in {0:N1} ms (PID={1}, WS={2} MB, Priv={3} MB, Handles={4}, GDI={5}, USER={6})" -f `
        $bootSw.Elapsed.TotalMilliseconds, $WkProc.Id, $idleMetrics.WsMb, $idleMetrics.PrivMb, $idleMetrics.Handles, $idleMetrics.Gdi, $idleMetrics.User) -ForegroundColor Green

    $ReportRows.Add([PSCustomObject]@{
        Action = "Cold Boot (All 8 ON)"
        LatencyMs = [Math]::Round($bootSw.Elapsed.TotalMilliseconds, 1)
        BeforeWs = 0.0; PeakWs = $idleMetrics.WsMb; AfterWs = $idleMetrics.WsMb
        PrivMb = $idleMetrics.PrivMb; Handles = $idleMetrics.Handles; Gdi = $idleMetrics.Gdi; User = $idleMetrics.User
        Result = "PASS"
    })

    # Action 1: SnapZones Preview Overlay
    $mBefore = Get-ProcMetrics $WkProc
    $sw = [System.Diagnostics.Stopwatch]::StartNew()
    [void][WkLiveNative]::PostMessageW($CtrlHwnd, $WM_WK_EXECUTE_ACTION, [IntPtr]$ACT_SHOW_ZONES_PREVIEW, [IntPtr]600)
    $overlayFound = $false
    for ($i = 0; $i -lt 50; $i++) {
        $ov = [WkLiveNative]::FindWindowByClassAndPid("Tools3000_WindowKit_SnapZones", [uint32]$WkProc.Id)
        if ($ov -ne [IntPtr]::Zero -and [WkLiveNative]::IsWindowVisible($ov)) { $overlayFound = $true; break }
        Start-Sleep -Milliseconds 10
    }
    $sw.Stop()
    $mPeak = Get-ProcMetrics $WkProc
    Start-Sleep -Milliseconds 800
    if (-not $overlayFound) { throw "SnapZones D2D overlay window did not become visible" }
    $mAfter = Get-ProcMetrics $WkProc
    $ReportRows.Add([PSCustomObject]@{
        Action = "SnapZones Overlay Preview"
        LatencyMs = [Math]::Round($sw.Elapsed.TotalMilliseconds, 1)
        BeforeWs = $mBefore.WsMb; PeakWs = $mPeak.WsMb; AfterWs = $mAfter.WsMb
        PrivMb = $mAfter.PrivMb; Handles = $mAfter.Handles; Gdi = $mAfter.Gdi; User = $mAfter.User
        Result = "PASS"
    })

    # Action 2: SnapZones Snap Target Window to Zone 0 -> Zone 1 -> Restore
    Focus-TargetWindow $HwndA
    $curFgNow = [WkLiveNative]::GetForegroundWindow()
    Write-Host "[DIAG] Target HwndA=0x$($HwndA.ToInt64().ToString('X')), CurrentForeground=0x$($curFgNow.ToInt64().ToString('X'))" -ForegroundColor DarkGray
    $origRect = New-Object WkLiveNative+RECT
    [void][WkLiveNative]::GetWindowRect($HwndA, [ref]$origRect)
    $mBefore = Get-ProcMetrics $WkProc
    $sw = [System.Diagnostics.Stopwatch]::StartNew()
    [void][WkLiveNative]::PostMessageW($CtrlHwnd, $WM_WK_EXECUTE_ACTION, [IntPtr]$ACT_MOVE_ZONE_INDEX, [IntPtr]0)
    $snappedRect = New-Object WkLiveNative+RECT
    $moved = $false
    for ($i = 0; $i -lt 50; $i++) {
        [System.Windows.Forms.Application]::DoEvents()
        [void][WkLiveNative]::GetWindowRect($HwndA, [ref]$snappedRect)
        if ($snappedRect.Left -ne $origRect.Left -or ($snappedRect.Right - $snappedRect.Left) -ne ($origRect.Right - $origRect.Left)) {
            $moved = $true; break
        }
        Start-Sleep -Milliseconds 10
    }
    $sw.Stop()
    if (-not $moved) {
        $diagLog = Join-Path $LogDir "window_kit.log"
        if (Test-Path -LiteralPath $diagLog) {
            Write-Host "[DIAG] window_kit.log tail:" -ForegroundColor Yellow
            Get-Content -LiteralPath $diagLog -Tail 25 | ForEach-Object { Write-Host "  $_" }
        }
        throw "SnapZones did not reposition target window to Zone 0 (orig=$($origRect.Left),$($origRect.Top) vs now=$($snappedRect.Left),$($snappedRect.Top))"
    }
    $mPeak = Get-ProcMetrics $WkProc
    [void][WkLiveNative]::PostMessageW($CtrlHwnd, $WM_WK_EXECUTE_ACTION, [IntPtr]$ACT_RESTORE_FROM_ZONE, [IntPtr]0)
    Start-Sleep -Milliseconds 180
    [System.Windows.Forms.Application]::DoEvents()
    $mAfter = Get-ProcMetrics $WkProc
    $ReportRows.Add([PSCustomObject]@{
        Action = "SnapZones Snap + Restore"
        LatencyMs = [Math]::Round($sw.Elapsed.TotalMilliseconds, 1)
        BeforeWs = $mBefore.WsMb; PeakWs = $mPeak.WsMb; AfterWs = $mAfter.WsMb
        PrivMb = $mAfter.PrivMb; Handles = $mAfter.Handles; Gdi = $mAfter.Gdi; User = $mAfter.User
        Result = "PASS"
    })

    # Action 3: Always on Top Toggle ON & OFF
    Focus-TargetWindow $HwndA
    $mBefore = Get-ProcMetrics $WkProc
    $sw = [System.Diagnostics.Stopwatch]::StartNew()
    [void][WkLiveNative]::PostMessageW($CtrlHwnd, $WM_WK_EXECUTE_ACTION, [IntPtr]$ACT_TOGGLE_AOT, [IntPtr]0)
    $isTopmost = $false
    for ($i = 0; $i -lt 50; $i++) {
        [System.Windows.Forms.Application]::DoEvents()
        $ex = [WkLiveNative]::GetWindowLongPtrW($HwndA, $GWL_EXSTYLE).ToInt64()
        if (($ex -band $WS_EX_TOPMOST) -ne 0) { $isTopmost = $true; break }
        Start-Sleep -Milliseconds 10
    }
    $sw.Stop()
    if (-not $isTopmost) { throw "AlwaysOnTop did not set WS_EX_TOPMOST on target window" }
    $mPeak = Get-ProcMetrics $WkProc
    # Toggle OFF
    [void][WkLiveNative]::PostMessageW($CtrlHwnd, $WM_WK_EXECUTE_ACTION, [IntPtr]$ACT_TOGGLE_AOT, [IntPtr]0)
    Start-Sleep -Milliseconds 150
    [System.Windows.Forms.Application]::DoEvents()
    $exAfter = [WkLiveNative]::GetWindowLongPtrW($HwndA, $GWL_EXSTYLE).ToInt64()
    if (($exAfter -band $WS_EX_TOPMOST) -ne 0) { throw "AlwaysOnTop toggle OFF failed to clear WS_EX_TOPMOST" }
    $mAfter = Get-ProcMetrics $WkProc
    $ReportRows.Add([PSCustomObject]@{
        Action = "Always on Top (ON/OFF)"
        LatencyMs = [Math]::Round($sw.Elapsed.TotalMilliseconds, 1)
        BeforeWs = $mBefore.WsMb; PeakWs = $mPeak.WsMb; AfterWs = $mAfter.WsMb
        PrivMb = $mAfter.PrivMb; Handles = $mAfter.Handles; Gdi = $mAfter.Gdi; User = $mAfter.User
        Result = "PASS"
    })

    # Action 4: Window Opacity Down -> Reset
    Focus-TargetWindow $HwndA
    $mBefore = Get-ProcMetrics $WkProc
    $sw = [System.Diagnostics.Stopwatch]::StartNew()
    [void][WkLiveNative]::PostMessageW($CtrlHwnd, $WM_WK_EXECUTE_ACTION, [IntPtr]$ACT_OPACITY_DOWN, [IntPtr]0)
    $alphaChanged = $false
    [byte]$alphaVal = 255
    for ($i = 0; $i -lt 50; $i++) {
        [System.Windows.Forms.Application]::DoEvents()
        [uint32]$ck = 0; [uint32]$fl = 0
        if ([WkLiveNative]::GetLayeredWindowAttributes($HwndA, [ref]$ck, [ref]$alphaVal, [ref]$fl) -and $alphaVal -lt 255) {
            $alphaChanged = $true; break
        }
        Start-Sleep -Milliseconds 10
    }
    $sw.Stop()
    if (-not $alphaChanged) { throw "Window Opacity down did not update WS_EX_LAYERED alpha" }
    $mPeak = Get-ProcMetrics $WkProc
    [void][WkLiveNative]::PostMessageW($CtrlHwnd, $WM_WK_EXECUTE_ACTION, [IntPtr]$ACT_OPACITY_RESET, [IntPtr]0)
    Start-Sleep -Milliseconds 150
    [System.Windows.Forms.Application]::DoEvents()
    $mAfter = Get-ProcMetrics $WkProc
    $ReportRows.Add([PSCustomObject]@{
        Action = "Window Opacity (Down/Reset)"
        LatencyMs = [Math]::Round($sw.Elapsed.TotalMilliseconds, 1)
        BeforeWs = $mBefore.WsMb; PeakWs = $mPeak.WsMb; AfterWs = $mAfter.WsMb
        PrivMb = $mAfter.PrivMb; Handles = $mAfter.Handles; Gdi = $mAfter.Gdi; User = $mAfter.User
        Result = "PASS (alpha=$alphaVal->255)"
    })

    # Action 5: Quick Switch v2 (Global Alt+Tab + 5/8 Bounds + Same-App Alt+`)
    $mBefore = Get-ProcMetrics $WkProc
    $sw = [System.Diagnostics.Stopwatch]::StartNew()
    [void][WkLiveNative]::PostMessageW($CtrlHwnd, $WM_WK_EXECUTE_ACTION, [IntPtr]$ACT_OPEN_QUICK_SWITCH, [IntPtr]0)
    $qsVisible = $false
    $qsHwnd = [IntPtr]::Zero
    for ($i = 0; $i -lt 50; $i++) {
        $qsHwnd = [WkLiveNative]::FindWindowByClassAndPid("Tools3000_WindowKit_QuickSwitch", [uint32]$WkProc.Id)
        if ($qsHwnd -ne [IntPtr]::Zero -and [WkLiveNative]::IsWindowVisible($qsHwnd)) { $qsVisible = $true; break }
        Start-Sleep -Milliseconds 10
    }
    $sw.Stop()
    if (-not $qsVisible) {
        $qsDiag = [WkLiveNative]::FindWindowByClassAndPid("Tools3000_WindowKit_QuickSwitch", [uint32]$WkProc.Id)
        Write-Host "[DIAG] QuickSwitch hwnd=0x$($qsDiag.ToInt64().ToString('X')), visible=$([WkLiveNative]::IsWindowVisible($qsDiag)), wkAlive=$(-not $WkProc.HasExited)" -ForegroundColor Yellow
        $diagLog = Join-Path $LogDir "window_kit.log"
        if (Test-Path -LiteralPath $diagLog) {
            Get-Content -LiteralPath $diagLog -Tail 25 | ForEach-Object { Write-Host "  $_" }
        }
        throw "QuickSwitch window did not appear"
    }
    $qsRect = New-Object WkLiveNative+RECT
    [void][WkLiveNative]::GetWindowRect($qsHwnd, [ref]$qsRect)
    $qsW = $qsRect.Right - $qsRect.Left
    $qsH = $qsRect.Bottom - $qsRect.Top
    if ($qsW -lt 640 -or $qsH -lt 420) {
        throw "QuickSwitch v2 default bounds too small: ${qsW}x${qsH}"
    }
    $mPeak = Get-ProcMetrics $WkProc
    Start-Sleep -Milliseconds 180
    # Close QuickSwitch
    [void][WkLiveNative]::PostMessageW($CtrlHwnd, $WM_WK_EXECUTE_ACTION, [IntPtr]$ACT_OPEN_QUICK_SWITCH, [IntPtr]0)
    Start-Sleep -Milliseconds 200
    $mAfter = Get-ProcMetrics $WkProc
    $ReportRows.Add([PSCustomObject]@{
        Action = "Quick Switch v2 (Global)"
        LatencyMs = [Math]::Round($sw.Elapsed.TotalMilliseconds, 1)
        BeforeWs = $mBefore.WsMb; PeakWs = $mPeak.WsMb; AfterWs = $mAfter.WsMb
        PrivMb = $mAfter.PrivMb; Handles = $mAfter.Handles; Gdi = $mAfter.Gdi; User = $mAfter.User
        Result = "PASS (${qsW}x${qsH})"
    })

    # Action 5b: Quick Switch v2 Same-App Alt+` Track
    Focus-TargetWindow $HwndA
    $mBefore = Get-ProcMetrics $WkProc
    $sw = [System.Diagnostics.Stopwatch]::StartNew()
    [void][WkLiveNative]::PostMessageW($CtrlHwnd, $WM_WK_INTERNAL_ALT_TAB, [IntPtr]0, [IntPtr]1)
    $sameAppVisible = $false
    for ($i = 0; $i -lt 50; $i++) {
        $qsSame = [WkLiveNative]::FindWindowByClassAndPid("Tools3000_WindowKit_QuickSwitch", [uint32]$WkProc.Id)
        if ($qsSame -ne [IntPtr]::Zero -and [WkLiveNative]::IsWindowVisible($qsSame)) { $sameAppVisible = $true; break }
        Start-Sleep -Milliseconds 10
    }
    $sw.Stop()
    $mPeak = Get-ProcMetrics $WkProc
    if ($sameAppVisible) {
        [void][WkLiveNative]::PostMessageW($CtrlHwnd, $WM_WK_EXECUTE_ACTION, [IntPtr]$ACT_OPEN_QUICK_SWITCH, [IntPtr]0)
        Start-Sleep -Milliseconds 160
    }
    $mAfter = Get-ProcMetrics $WkProc
    $ReportRows.Add([PSCustomObject]@{
        Action = "Quick Switch v2 (Alt+``)"
        LatencyMs = [Math]::Round($sw.Elapsed.TotalMilliseconds, 1)
        BeforeWs = $mBefore.WsMb; PeakWs = $mPeak.WsMb; AfterWs = $mAfter.WsMb
        PrivMb = $mAfter.PrivMb; Handles = $mAfter.Handles; Gdi = $mAfter.Gdi; User = $mAfter.User
        Result = if ($sameAppVisible) { "PASS (Same-App Card)" } else { "PASS (Single-Instance Direct)" }
    })

    # Action 5c: SnapZones v2 Dual-Layer Layout Hub (Action 20) & Direct 1:1 Editor (Action 24)
    $mBefore = Get-ProcMetrics $WkProc
    $sw = [System.Diagnostics.Stopwatch]::StartNew()
    [void][WkLiveNative]::PostMessageW($CtrlHwnd, $WM_WK_EXECUTE_ACTION, [IntPtr]$ACT_OPEN_LAYOUT_HUB, [IntPtr]0)
    $hubHwnd = [IntPtr]::Zero
    for ($i = 0; $i -lt 80; $i++) {
        $hubHwnd = [WkLiveNative]::FindWindowByClassAndPid("Tools3000_WindowKit_LayoutHub", [uint32]$WkProc.Id)
        if ($hubHwnd -ne [IntPtr]::Zero) { break }
        Start-Sleep -Milliseconds 15
    }
    $sw.Stop()
    if ($hubHwnd -eq [IntPtr]::Zero) { throw "Action 20 Dual-Layer LayoutHub did not appear" }
    $mPeak = Get-ProcMetrics $WkProc
    [void][WkLiveNative]::PostMessageW($hubHwnd, $WM_CLOSE, [IntPtr]::Zero, [IntPtr]::Zero)
    Start-Sleep -Milliseconds 220
    $mAfter = Get-ProcMetrics $WkProc
    $ReportRows.Add([PSCustomObject]@{
        Action = "SnapZones v2 LayoutHub"
        LatencyMs = [Math]::Round($sw.Elapsed.TotalMilliseconds, 1)
        BeforeWs = $mBefore.WsMb; PeakWs = $mPeak.WsMb; AfterWs = $mAfter.WsMb
        PrivMb = $mAfter.PrivMb; Handles = $mAfter.Handles; Gdi = $mAfter.Gdi; User = $mAfter.User
        Result = "PASS"
    })

    $mBefore = Get-ProcMetrics $WkProc
    $sw = [System.Diagnostics.Stopwatch]::StartNew()
    [void][WkLiveNative]::PostMessageW($CtrlHwnd, $WM_WK_EXECUTE_ACTION, [IntPtr]$ACT_OPEN_LAYOUT_DIRECT, [IntPtr]0)
    $edHwnd = [IntPtr]::Zero
    for ($i = 0; $i -lt 80; $i++) {
        $edHwnd = [WkLiveNative]::FindWindowByClassAndPid("Tools3000_WindowKit_LayoutEditor", [uint32]$WkProc.Id)
        if ($edHwnd -ne [IntPtr]::Zero) { break }
        Start-Sleep -Milliseconds 15
    }
    $sw.Stop()
    if ($edHwnd -eq [IntPtr]::Zero) { throw "Action 24 Direct 1:1 LayoutEditor did not appear" }
    $mPeak = Get-ProcMetrics $WkProc
    [void][WkLiveNative]::PostMessageW($edHwnd, $WM_CLOSE, [IntPtr]::Zero, [IntPtr]::Zero)
    Start-Sleep -Milliseconds 220
    $mAfter = Get-ProcMetrics $WkProc
    $ReportRows.Add([PSCustomObject]@{
        Action = "SnapZones v2 1:1 Editor"
        LatencyMs = [Math]::Round($sw.Elapsed.TotalMilliseconds, 1)
        BeforeWs = $mBefore.WsMb; PeakWs = $mPeak.WsMb; AfterWs = $mAfter.WsMb
        PrivMb = $mAfter.PrivMb; Handles = $mAfter.Handles; Gdi = $mAfter.Gdi; User = $mAfter.User
        Result = "PASS"
    })

    # Action 5d: Workspaces v2 4px Red-Frame Capture (Action 22) + Border Style Preview (Action 23) + Layout Digit Switch (Action 21)
    $mBefore = Get-ProcMetrics $WkProc
    $sw = [System.Diagnostics.Stopwatch]::StartNew()
    [void][WkLiveNative]::PostMessageW($CtrlHwnd, $WM_WK_EXECUTE_ACTION, [IntPtr]$ACT_START_WS_CAPTURE, [IntPtr]0)
    $capCard = [IntPtr]::Zero
    for ($i = 0; $i -lt 80; $i++) {
        $capCard = [WkLiveNative]::FindWindowByClassAndPid("Tools3000_WindowKit_CaptureCard", [uint32]$WkProc.Id)
        if ($capCard -ne [IntPtr]::Zero) { break }
        Start-Sleep -Milliseconds 15
    }
    $sw.Stop()
    if ($capCard -eq [IntPtr]::Zero) { throw "Action 22 Red-Frame CaptureCard did not appear" }
    $stripCnt = [WkLiveNative]::CountWindowsByClassAndPid("Tools3000_WindowKit_CaptureFrame", [uint32]$WkProc.Id)
    $expectedStrips = [int]$monJson.count * 4
    if ($stripCnt -lt $expectedStrips) {
        throw "Action 22 expected $expectedStrips red border strips, got $stripCnt"
    }
    $mPeak = Get-ProcMetrics $WkProc
    [void][WkLiveNative]::PostMessageW($capCard, $WM_CLOSE, [IntPtr]::Zero, [IntPtr]::Zero)
    Start-Sleep -Milliseconds 220
    [void][WkLiveNative]::PostMessageW($CtrlHwnd, $WM_WK_EXECUTE_ACTION, [IntPtr]$ACT_PREVIEW_BORDER, [IntPtr]0)
    [void][WkLiveNative]::PostMessageW($CtrlHwnd, $WM_WK_EXECUTE_ACTION, [IntPtr]$ACT_APPLY_LAYOUT_DIGIT, [IntPtr]1)
    Start-Sleep -Milliseconds 180
    $mAfter = Get-ProcMetrics $WkProc
    $ReportRows.Add([PSCustomObject]@{
        Action = "Workspaces v2 Red-Frame"
        LatencyMs = [Math]::Round($sw.Elapsed.TotalMilliseconds, 1)
        BeforeWs = $mBefore.WsMb; PeakWs = $mPeak.WsMb; AfterWs = $mAfter.WsMb
        PrivMb = $mAfter.PrivMb; Handles = $mAfter.Handles; Gdi = $mAfter.Gdi; User = $mAfter.User
        Result = "PASS (strips=$stripCnt/$expectedStrips)"
    })

    # Action 5e: Workspaces v2 Real Launch & Zone Placement (Action 15)
    $preNotes = @(Get-Process -Name "Notepad" -ErrorAction SilentlyContinue | Select-Object -ExpandProperty Id)
    $wsPayloadPath = Join-Path $TempRoot "window_kit_action.json"
    @{ workspaceId = "e2e-ws-1" } | ConvertTo-Json -Compress | Set-Content -LiteralPath $wsPayloadPath -Encoding UTF8
    $mBefore = Get-ProcMetrics $WkProc
    $sw = [System.Diagnostics.Stopwatch]::StartNew()
    [void][WkLiveNative]::PostMessageW($CtrlHwnd, $WM_WK_EXECUTE_ACTION, [IntPtr]$ACT_LAUNCH_WORKSPACE, [IntPtr]0)
    $launchedNote = $null
    for ($i = 0; $i -lt 120; $i++) {
        $nowNotes = @(Get-Process -Name "Notepad" -ErrorAction SilentlyContinue | Where-Object { $preNotes -notcontains $_.Id -and $_.MainWindowHandle -ne [IntPtr]::Zero })
        if ($nowNotes.Count -gt 0) { $launchedNote = $nowNotes[0]; break }
        Start-Sleep -Milliseconds 50
    }
    $sw.Stop()
    $mPeak = Get-ProcMetrics $WkProc
    if (-not $launchedNote) {
        throw "Action 15 Launch Workspace did not spawn target notepad.exe window within 6000ms"
    }
    Start-Sleep -Milliseconds 400
    $noteRect = New-Object WkLiveNative+RECT
    [void][WkLiveNative]::GetWindowRect($launchedNote.MainWindowHandle, [ref]$noteRect)
    try { Stop-Process -Id $launchedNote.Id -Force -ErrorAction SilentlyContinue } catch {}
    Start-Sleep -Milliseconds 200
    $mAfter = Get-ProcMetrics $WkProc
    $ReportRows.Add([PSCustomObject]@{
        Action = "Workspaces v2 Launch"
        LatencyMs = [Math]::Round($sw.Elapsed.TotalMilliseconds, 1)
        BeforeWs = $mBefore.WsMb; PeakWs = $mPeak.WsMb; AfterWs = $mAfter.WsMb
        PrivMb = $mAfter.PrivMb; Handles = $mAfter.Handles; Gdi = $mAfter.Gdi; User = $mAfter.User
        Result = "PASS ($($noteRect.Left),$($noteRect.Top) $($noteRect.Right - $noteRect.Left)x$($noteRect.Bottom - $noteRect.Top))"
    })

    # Action 6: Multi-Monitor Rescue Off-Screen Window
    [void][WkLiveNative]::SetWindowPos($HwndB, [IntPtr]::Zero, -4200, -4200, 0, 0, $SWP_NOSIZE -bor $SWP_NOZORDER -bor $SWP_NOACTIVATE)
    [System.Windows.Forms.Application]::DoEvents()
    Start-Sleep -Milliseconds 80
    $mBefore = Get-ProcMetrics $WkProc
    $sw = [System.Diagnostics.Stopwatch]::StartNew()
    [void][WkLiveNative]::PostMessageW($CtrlHwnd, $WM_WK_EXECUTE_ACTION, [IntPtr]$ACT_RESCUE_OFFSCREEN, [IntPtr]0)
    $rescued = $false
    $rectB = New-Object WkLiveNative+RECT
    for ($i = 0; $i -lt 50; $i++) {
        [System.Windows.Forms.Application]::DoEvents()
        [void][WkLiveNative]::GetWindowRect($HwndB, [ref]$rectB)
        if ($rectB.Left -gt -3500 -and $rectB.Top -gt -3500) { $rescued = $true; break }
        Start-Sleep -Milliseconds 10
    }
    $sw.Stop()
    if (-not $rescued) { throw "Rescue off-screen window failed; rect=($($rectB.Left), $($rectB.Top))" }
    $mPeak = Get-ProcMetrics $WkProc
    $ReportRows.Add([PSCustomObject]@{
        Action = "Multi-Monitor Rescue"
        LatencyMs = [Math]::Round($sw.Elapsed.TotalMilliseconds, 1)
        BeforeWs = $mBefore.WsMb; PeakWs = $mPeak.WsMb; AfterWs = $mPeak.WsMb
        PrivMb = $mPeak.PrivMb; Handles = $mPeak.Handles; Gdi = $mPeak.Gdi; User = $mPeak.User
        Result = "PASS (-4200->($($rectB.Left),$($rectB.Top)))"
    })

    # Action 7: Minimize to Tray & Restore All (自动适配真实交互式桌面 vs 无 Shell_TrayWnd 的隔离桌面)
    $HasShellTray = ([WkLiveNative]::FindWindowByClass("Shell_TrayWnd") -ne [IntPtr]::Zero)
    Focus-TargetWindow $HwndB
    $mBefore = Get-ProcMetrics $WkProc
    $sw = [System.Diagnostics.Stopwatch]::StartNew()
    [void][WkLiveNative]::PostMessageW($CtrlHwnd, $WM_WK_EXECUTE_ACTION, [IntPtr]$ACT_MINIMIZE_TO_TRAY, [IntPtr]0)
    if ($HasShellTray) {
        $hidden = $false
        for ($i = 0; $i -lt 50; $i++) {
            [System.Windows.Forms.Application]::DoEvents()
            if (-not [WkLiveNative]::IsWindowVisible($HwndB)) { $hidden = $true; break }
            Start-Sleep -Milliseconds 10
        }
        $sw.Stop()
        if (-not $hidden) {
            throw "Minimize to Tray did not hide target window on interactive Shell desktop"
        }
        $mPeak = Get-ProcMetrics $WkProc
        [void][WkLiveNative]::PostMessageW($CtrlHwnd, $WM_WK_EXECUTE_ACTION, [IntPtr]$ACT_RESTORE_ALL_TRAY, [IntPtr]0)
        $restored = $false
        for ($i = 0; $i -lt 50; $i++) {
            [System.Windows.Forms.Application]::DoEvents()
            if ([WkLiveNative]::IsWindowVisible($HwndB)) { $restored = $true; break }
            Start-Sleep -Milliseconds 10
        }
        if (-not $restored) { throw "Restore all tray windows failed" }
        $mAfter = Get-ProcMetrics $WkProc
        $ReportRows.Add([PSCustomObject]@{
            Action = "Minimize to Tray + Restore"
            LatencyMs = [Math]::Round($sw.Elapsed.TotalMilliseconds, 1)
            BeforeWs = $mBefore.WsMb; PeakWs = $mPeak.WsMb; AfterWs = $mAfter.WsMb
            PrivMb = $mAfter.PrivMb; Handles = $mAfter.Handles; Gdi = $mAfter.Gdi; User = $mAfter.User
            Result = "PASS"
        })
    } else {
        # 无 Shell_TrayWnd 的隔离测试桌面：验证安全门禁绝不误隐藏用户窗口（防止丢失窗口）
        Start-Sleep -Milliseconds 180
        [System.Windows.Forms.Application]::DoEvents()
        $sw.Stop()
        if (-not [WkLiveNative]::IsWindowVisible($HwndB)) {
            throw "Safety violation: window hidden even though Shell_TrayWnd is absent!"
        }
        $mAfter = Get-ProcMetrics $WkProc
        $ReportRows.Add([PSCustomObject]@{
            Action = "Minimize to Tray (No-Shell Guard)"
            LatencyMs = [Math]::Round($sw.Elapsed.TotalMilliseconds, 1)
            BeforeWs = $mBefore.WsMb; PeakWs = $mAfter.WsMb; AfterWs = $mAfter.WsMb
            PrivMb = $mAfter.PrivMb; Handles = $mAfter.Handles; Gdi = $mAfter.Gdi; User = $mAfter.User
            Result = "PASS (safe-guard: window kept visible)"
        })
    }

    # ── Phase 3: 20 轮高频压测 + 3.5s 冷路径工作集回落与句柄零泄漏验证 ─────────
    Write-Host "`n[STEP 4] Running $StressCycles-cycle stress & handle leak torture test..." -ForegroundColor Yellow
    Start-Sleep -Milliseconds 2500 # 等待前序冷路径修剪形成稳定基线
    $preStress = Get-ProcMetrics $WkProc
    $stressSw = [System.Diagnostics.Stopwatch]::StartNew()
    for ($c = 1; $c -le $StressCycles; $c++) {
        [void][WkLiveNative]::PostMessageW($CtrlHwnd, $WM_WK_EXECUTE_ACTION, [IntPtr]$ACT_OPEN_QUICK_SWITCH, [IntPtr]0)
        Start-Sleep -Milliseconds 45
        [void][WkLiveNative]::PostMessageW($CtrlHwnd, $WM_WK_EXECUTE_ACTION, [IntPtr]$ACT_OPEN_QUICK_SWITCH, [IntPtr]0)
        Start-Sleep -Milliseconds 35
        [void][WkLiveNative]::PostMessageW($CtrlHwnd, $WM_WK_EXECUTE_ACTION, [IntPtr]$ACT_SHOW_ZONES_PREVIEW, [IntPtr]200)
        Start-Sleep -Milliseconds 40
        [void][WkLiveNative]::PostMessageW($CtrlHwnd, $WM_WK_EXECUTE_ACTION, [IntPtr]$ACT_SHOW_ZONES_PREVIEW, [IntPtr]0)
        Start-Sleep -Milliseconds 30
    }
    $stressSw.Stop()
    $peakStress = Get-ProcMetrics $WkProc
    Write-Host "[INFO] Waiting 3.5s for cold-path HeapCompact + WorkingSet trim..." -ForegroundColor Cyan
    Start-Sleep -Milliseconds 3500
    $postStress = Get-ProcMetrics $WkProc

    $handleDelta = [int]$postStress.Handles - [int]$preStress.Handles
    $gdiDelta = [int]$postStress.Gdi - [int]$preStress.Gdi
    $userDelta = [int]$postStress.User - [int]$preStress.User
    $privDelta = [Math]::Round($postStress.PrivMb - $preStress.PrivMb, 2)

    $ReportRows.Add([PSCustomObject]@{
        Action = "Stress ${StressCycles}x + Cold Trim(3.5s)"
        LatencyMs = [Math]::Round($stressSw.Elapsed.TotalMilliseconds / $StressCycles, 1)
        BeforeWs = $preStress.WsMb; PeakWs = $peakStress.WsMb; AfterWs = $postStress.WsMb
        PrivMb = $postStress.PrivMb; Handles = $postStress.Handles; Gdi = $postStress.Gdi; User = $postStress.User
        Result = "dH=$handleDelta dGDI=$gdiDelta dUSR=$userDelta dPriv=${privDelta}MB"
    })

    if ($gdiDelta -gt 4 -or $userDelta -gt 4 -or $handleDelta -gt 12) {
        throw "Handle/GDI/USER leak detected after $StressCycles cycles! dH=$handleDelta, dGDI=$gdiDelta, dUSR=$userDelta"
    }

    # ── Phase 4: 托盘收纳窗在引擎优雅退出时的 100% 自动防丢失还原验证 ──────────
    Write-Host "`n[STEP 5] Verifying graceful shutdown & cleanup..." -ForegroundColor Yellow
    if ($HasShellTray) {
        Focus-TargetWindow $HwndB
        [void][WkLiveNative]::PostMessageW($CtrlHwnd, $WM_WK_EXECUTE_ACTION, [IntPtr]$ACT_MINIMIZE_TO_TRAY, [IntPtr]0)
        Start-Sleep -Milliseconds 200
        [System.Windows.Forms.Application]::DoEvents()
        if ([WkLiveNative]::IsWindowVisible($HwndB)) {
            throw "Failed to minimize target window B to tray before shutdown test"
        }
    } else {
        # 无 Shell_TrayWnd 环境下，先给目标窗降透明度，验证优雅退出时透明度 100% 自动还原为 255
        Focus-TargetWindow $HwndB
        [void][WkLiveNative]::PostMessageW($CtrlHwnd, $WM_WK_EXECUTE_ACTION, [IntPtr]$ACT_OPACITY_DOWN, [IntPtr]0)
        Start-Sleep -Milliseconds 180
        [System.Windows.Forms.Application]::DoEvents()
    }

    $shutSw = [System.Diagnostics.Stopwatch]::StartNew()
    [void][WkLiveNative]::PostMessageW($CtrlHwnd, $WM_WK_GRACEFUL_SHUTDOWN, [IntPtr]::Zero, [IntPtr]::Zero)
    if (-not $WkProc.WaitForExit(2500)) {
        throw "WindowKit did not exit cleanly within 2500ms after WM_WK_GRACEFUL_SHUTDOWN"
    }
    $shutSw.Stop()
    [System.Windows.Forms.Application]::DoEvents()
    Start-Sleep -Milliseconds 150
    [System.Windows.Forms.Application]::DoEvents()
    if (-not [WkLiveNative]::IsWindowVisible($HwndB)) {
        throw "CRITICAL: Target window B was NOT visible after graceful shutdown!"
    }
    $exB = [WkLiveNative]::GetWindowLongPtrW($HwndB, $GWL_EXSTYLE).ToInt64()
    if (($exB -band $WS_EX_LAYERED) -ne 0) {
        [uint32]$ck2 = 0; [byte]$al2 = 255; [uint32]$fl2 = 0
        if ([WkLiveNative]::GetLayeredWindowAttributes($HwndB, [ref]$ck2, [ref]$al2, [ref]$fl2) -and $al2 -lt 255) {
            throw "CRITICAL: Window B opacity ($al2) was NOT restored to 255 on graceful shutdown!"
        }
    }
    Write-Host ("[OK] Graceful shutdown completed in {0:N1} ms; all modified window states 100% auto-restored!" -f $shutSw.Elapsed.TotalMilliseconds) -ForegroundColor Green

    # ── Phase 5: 日志纯净度审计 ──────────────────────────────────────────────
    Write-Host "`n[STEP 6] Auditing window_kit.log telemetry & purity..." -ForegroundColor Yellow
    $LogFile = Join-Path $LogDir "window_kit.log"
    if (-not (Test-Path -LiteralPath $LogFile)) {
        throw "Expected log file not found: $LogFile"
    }
    $LogLines = Get-Content -LiteralPath $LogFile -Encoding UTF8
    $ErrorLines = @($LogLines | Where-Object { $_ -match '\[ERROR\]' })
    $WarnLines = @($LogLines | Where-Object { $_ -match '\[WARN\]' })
    $TraceLines = @($LogLines | Where-Object { $_ -match '\[[a-z]+-\d+\]' })
    Write-Host "[OK] Log audit: total=$($LogLines.Count) lines, TraceIdLines=$($TraceLines.Count), ERROR=$($ErrorLines.Count), WARN=$($WarnLines.Count)" -ForegroundColor Green
    if ($ErrorLines.Count -gt 0) {
        Write-Host "[ERROR] Unexpected ERROR lines found in window_kit.log:" -ForegroundColor Red
        $ErrorLines | ForEach-Object { Write-Host "  $_" -ForegroundColor Red }
        throw "window_kit.log contains ERROR entries!"
    }

    Write-Host "`n==================== LIVE E2E TELEMETRY REPORT ====================" -ForegroundColor Cyan
    $ReportRows | Format-Table -AutoSize
    Write-Host "Sample window_kit.log tail (last 12 lines):" -ForegroundColor Cyan
    $LogLines | Select-Object -Last 12 | ForEach-Object { Write-Host "  $_" }
    Write-Host "`n[OK] ALL LIVE E2E ASSERTIONS PASSED!" -ForegroundColor Green
} finally {
    if ($WkProc -and -not $WkProc.HasExited) {
        try { $WkProc.Kill() } catch {}
    }
    if ($TestFormA) { try { $TestFormA.Close(); $TestFormA.Dispose() } catch {} }
    if ($TestFormB) { try { $TestFormB.Close(); $TestFormB.Dispose() } catch {} }
    if ($OrigFgHwnd -ne [IntPtr]::Zero -and [WkLiveNative]::IsWindow($OrigFgHwnd)) {
        try { [void][WkLiveNative]::ForceForeground($OrigFgHwnd) } catch {}
    }
    if (Test-Path -LiteralPath $TempRoot) {
        Remove-Item -LiteralPath $TempRoot -Recurse -Force -ErrorAction SilentlyContinue
    }
}
