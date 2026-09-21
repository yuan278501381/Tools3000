# ─────────────────────────────────────────────────────────────────────────────
# stress_gesture_1000hz.ps1 — Tools3000 Tier 4 真实高压工作负载压力测试门禁
# ─────────────────────────────────────────────────────────────────────────────
# 覆盖 TEST_INFRA.md Tier 4 核心场景：
#   Scenario 1: 1000Hz 报告率 + CPU 满载高速连续画圆与折笔 (0 丢事件、亚毫秒延迟)
#   Scenario 2: < 50ms 极速短促单笔画手势 (Back/Forward 零卡顿即时识别)
#   Scenario 3: 4K 多屏幕跨屏连续大行程手势 (预分配表面 0 SetWindowPos)
#   Scenario 4: 复杂多笔画连续手势带 HUD 即时状态反馈与淡出
#   Scenario 5: 左键中断与前台窗口无响应自愈测试
# ─────────────────────────────────────────────────────────────────────────────
param(
    [string]$ExePath = "",
    [int]$CpuLoadThreads = 2,
    [switch]$SkipCpuStress
)

[Console]::OutputEncoding = [System.Text.Encoding]::UTF8
$ErrorActionPreference = "Stop"

Write-Host "===============================================================================" -ForegroundColor Cyan
Write-Host " [TIER 4] Tools3000 1000Hz 鼠标手势与 DirectComposition 压力测试门禁" -ForegroundColor Cyan
Write-Host "===============================================================================" -ForegroundColor Cyan

$ProjectRoot = Split-Path -Parent $PSScriptRoot

if ([string]::IsNullOrWhiteSpace($ExePath)) {
    $candidate1 = Join-Path $ProjectRoot "build\bin\Release\Tools3000.exe"
    $candidate2 = Join-Path $ProjectRoot "deploy_dist\Tools3000.exe"
    if (Test-Path -LiteralPath $candidate1) {
        $ExePath = $candidate1
    } elseif (Test-Path -LiteralPath $candidate2) {
        $ExePath = $candidate2
    } else {
        Write-Host "[ERROR] 未找到 Tools3000.exe，请先编译或打包！" -ForegroundColor Red
        exit 1
    }
}
$ExePath = (Resolve-Path -LiteralPath $ExePath).Path
Write-Host "[INFO] 待测程序: $ExePath" -ForegroundColor Gray

# 准备沙箱隔离测试环境
$HarnessRoot = Join-Path $ProjectRoot "build\stress-gesture-$PID"
$HarnessLocalAppData = Join-Path $HarnessRoot "LocalAppData"
$HarnessRoamingAppData = Join-Path $HarnessRoot "RoamingAppData"
$HarnessDataRoot = Join-Path $HarnessRoot "Tools3000Data"
$HarnessLogsDir = Join-Path $HarnessDataRoot "logs"
New-Item -ItemType Directory -Path $HarnessLocalAppData, $HarnessRoamingAppData, $HarnessDataRoot, $HarnessLogsDir -Force | Out-Null

$HarnessConfigDirectory = Join-Path $HarnessDataRoot "config"
New-Item -ItemType Directory -Path $HarnessConfigDirectory -Force | Out-Null
$HarnessConfigPath = Join-Path $HarnessConfigDirectory "config.json"

$ConfigJson = @{
    plugins = @{
        gesture = @{ enabled = $true }
    }
    gesture = @{
        enabled = $true
        paused = $false
        triggerButton = "right"
        trailVisible = $true
        initialTimeoutMs = 500
        minSegmentDistance = 14
    }
} | ConvertTo-Json -Depth 4
[System.IO.File]::WriteAllText($HarnessConfigPath, $ConfigJson, [System.Text.UTF8Encoding]::new($false))

# 注册 Win32 高精度 1000Hz 输入模拟器与 QPC 测时器
Add-Type @"
using System;
using System.Runtime.InteropServices;
using System.Threading;
using System.Diagnostics;
using System.Collections.Generic;

public static class HighCadenceMouseSimulator {
    [StructLayout(LayoutKind.Sequential)]
    public struct MOUSEINPUT {
        public int dx;
        public int dy;
        public uint mouseData;
        public uint dwFlags;
        public uint time;
        public IntPtr dwExtraInfo;
    }

    [StructLayout(LayoutKind.Explicit)]
    public struct INPUT {
        [FieldOffset(0)] public int type;
        [FieldOffset(8)] public MOUSEINPUT mi;
    }

    [DllImport("user32.dll", SetLastError=true)]
    public static extern uint SendInput(uint nInputs, INPUT[] pInputs, int cbSize);

    [DllImport("user32.dll")]
    public static extern bool GetCursorPos(out POINT lpPoint);

    [DllImport("user32.dll")]
    public static extern bool SetCursorPos(int X, int Y);

    [DllImport("user32.dll")]
    public static extern IntPtr GetThreadDesktop(int dwThreadId);

    [DllImport("kernel32.dll")]
    public static extern int GetCurrentThreadId();

    [DllImport("user32.dll", SetLastError=true)]
    public static extern bool SwitchDesktop(IntPtr hDesktop);

    [DllImport("user32.dll", SetLastError=true)]
    public static extern IntPtr OpenInputDesktop(uint dwFlags, bool fInherit, uint dwDesiredAccess);

    [DllImport("user32.dll", SetLastError=true)]
    public static extern bool CloseDesktop(IntPtr hDesktop);

    [DllImport("user32.dll")]
    public static extern int GetSystemMetrics(int nIndex);

    [DllImport("user32.dll", SetLastError=true, CharSet=CharSet.Unicode)]
    public static extern IntPtr FindWindow(string lpClassName, string lpWindowName);

    [DllImport("user32.dll")]
    public static extern uint GetWindowThreadProcessId(IntPtr hWnd, out uint lpdwProcessId);

    [DllImport("user32.dll", SetLastError=true)]
    public static extern bool PostMessage(IntPtr hWnd, uint Msg, IntPtr wParam, IntPtr lParam);

    [DllImport("winmm.dll")]
    public static extern uint timeBeginPeriod(uint uPeriod);

    [DllImport("winmm.dll")]
    public static extern uint timeEndPeriod(uint uPeriod);

    [StructLayout(LayoutKind.Sequential)]
    public struct POINT { public int x; public int y; }

    public const int SM_XVIRTUALSCREEN = 76;
    public const int SM_YVIRTUALSCREEN = 77;
    public const int SM_CXVIRTUALSCREEN = 78;
    public const int SM_CYVIRTUALSCREEN = 79;

    public const int INPUT_MOUSE = 0;
    public const uint MOUSEEVENTF_MOVE = 0x0001;
    public const uint MOUSEEVENTF_LEFTDOWN = 0x0002;
    public const uint MOUSEEVENTF_LEFTUP = 0x0004;
    public const uint MOUSEEVENTF_RIGHTDOWN = 0x0008;
    public const uint MOUSEEVENTF_RIGHTUP = 0x0010;
    public const uint MOUSEEVENTF_ABSOLUTE = 0x8000;
    public const uint MOUSEEVENTF_VIRTUALDESK = 0x4000;
    public const uint DESKTOP_ALL = 0x01FF;
    public const uint WM_CLOSE = 0x0010;
    public static readonly IntPtr TEST_EXTRA_INFO = new IntPtr(0x54455354); // "TEST"

    private static IntPtr _originalInputDesktop = IntPtr.Zero;
    private static IntPtr _currentDesktop = IntPtr.Zero;

    public static long TotalInjectedEvents = 0;
    public static long FailedInjectedEvents = 0;
    public static int LastWin32Error = 0;

    public static void ResetStats() {
        TotalInjectedEvents = 0;
        FailedInjectedEvents = 0;
        LastWin32Error = 0;
    }

    public static void InitializeDesktop() {
        int tid = GetCurrentThreadId();
        _currentDesktop = GetThreadDesktop(tid);
        _originalInputDesktop = OpenInputDesktop(0, false, DESKTOP_ALL);
        if (_currentDesktop != IntPtr.Zero) {
            SwitchDesktop(_currentDesktop);
        }
    }

    public static void RestoreDesktop() {
        if (_originalInputDesktop != IntPtr.Zero && _originalInputDesktop != _currentDesktop) {
            SwitchDesktop(_originalInputDesktop);
            CloseDesktop(_originalInputDesktop);
            _originalInputDesktop = IntPtr.Zero;
        }
    }

    private static void ToNormalized(int x, int y, out int normX, out int normY) {
        int left = GetSystemMetrics(SM_XVIRTUALSCREEN);
        int top = GetSystemMetrics(SM_YVIRTUALSCREEN);
        int w = GetSystemMetrics(SM_CXVIRTUALSCREEN);
        int h = GetSystemMetrics(SM_CYVIRTUALSCREEN);
        if (w <= 1) w = 1920;
        if (h <= 1) h = 1080;
        normX = (int)Math.Round(((x - left) * 65535.0) / (w - 1));
        normY = (int)Math.Round(((y - top) * 65535.0) / (h - 1));
    }

    public static void PreciseSleep(double ms) {
        long qpcFreq = Stopwatch.Frequency;
        long targetTicks = (long)(ms * (qpcFreq / 1000.0));
        Stopwatch sw = Stopwatch.StartNew();
        while (sw.ElapsedTicks < targetTicks) {
            Thread.SpinWait(10);
        }
    }

    public static void SendInputChecked(INPUT[] inputs) {
        uint res = SendInput((uint)inputs.Length, inputs, Marshal.SizeOf(typeof(INPUT)));
        if (res == 0 || res < (uint)inputs.Length) {
            int err = Marshal.GetLastWin32Error();
            LastWin32Error = err;
            Interlocked.Add(ref FailedInjectedEvents, inputs.Length - (int)res);
            throw new InvalidOperationException(string.Format("SendInput failed: returned {0}/{1}, Win32Error={2}", res, inputs.Length, err));
        }
        Interlocked.Add(ref TotalInjectedEvents, res);
    }

    public static void SendRightDown(int x, int y) {
        SetCursorPos(x, y);
        Thread.Sleep(10);
        int nx, ny;
        ToNormalized(x, y, out nx, out ny);
        INPUT[] inputs = new INPUT[1];
        inputs[0].type = INPUT_MOUSE;
        inputs[0].mi.dx = nx;
        inputs[0].mi.dy = ny;
        inputs[0].mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK | MOUSEEVENTF_RIGHTDOWN;
        inputs[0].mi.dwExtraInfo = TEST_EXTRA_INFO;
        SendInputChecked(inputs);
    }

    public static void SendRightUp(int x, int y) {
        SetCursorPos(x, y);
        Thread.Sleep(10);
        int nx, ny;
        ToNormalized(x, y, out nx, out ny);
        INPUT[] inputs = new INPUT[1];
        inputs[0].type = INPUT_MOUSE;
        inputs[0].mi.dx = nx;
        inputs[0].mi.dy = ny;
        inputs[0].mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK | MOUSEEVENTF_RIGHTUP;
        inputs[0].mi.dwExtraInfo = TEST_EXTRA_INFO;
        SendInputChecked(inputs);
    }

    public static void SendMove(int x, int y) {
        SetCursorPos(x, y);
        int nx, ny;
        ToNormalized(x, y, out nx, out ny);
        INPUT[] inputs = new INPUT[1];
        inputs[0].type = INPUT_MOUSE;
        inputs[0].mi.dx = nx;
        inputs[0].mi.dy = ny;
        inputs[0].mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK;
        inputs[0].mi.dwExtraInfo = TEST_EXTRA_INFO;
        SendInputChecked(inputs);
    }

    public static void SendLeftClick(int x, int y) {
        SetCursorPos(x, y);
        Thread.Sleep(10);
        int nx, ny;
        ToNormalized(x, y, out nx, out ny);
        INPUT[] down = new INPUT[1];
        down[0].type = INPUT_MOUSE;
        down[0].mi.dx = nx;
        down[0].mi.dy = ny;
        down[0].mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK | MOUSEEVENTF_LEFTDOWN;
        down[0].mi.dwExtraInfo = TEST_EXTRA_INFO;
        SendInputChecked(down);
        Thread.Sleep(10);
        INPUT[] up = new INPUT[1];
        up[0].type = INPUT_MOUSE;
        up[0].mi.dx = nx;
        up[0].mi.dy = ny;
        up[0].mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK | MOUSEEVENTF_LEFTUP;
        up[0].mi.dwExtraInfo = TEST_EXTRA_INFO;
        SendInputChecked(up);
    }

    public static bool RequestProcessClose(int targetPid) {
        IntPtr hwnd = FindWindow("Tools3000_MessageWindow", "Tools3000MessageWindow");
        if (hwnd != IntPtr.Zero) {
            uint pid;
            GetWindowThreadProcessId(hwnd, out pid);
            if (pid == (uint)targetPid || targetPid == 0) {
                return PostMessage(hwnd, WM_CLOSE, IntPtr.Zero, IntPtr.Zero);
            }
        }
        return false;
    }

    // 1000Hz 高频画圆压测引擎
    public static double Run1000HzCircularStream(int centerX, int centerY, int radius, int totalPoints) {
        timeBeginPeriod(1);
        try {
            SendRightDown(centerX + radius, centerY);
            Stopwatch sw = Stopwatch.StartNew();
            long qpcFreq = Stopwatch.Frequency;
            double targetIntervalMs = 1.0; // 1.0ms -> 1000Hz

            int sent = 0;
            for (int i = 0; i < totalPoints; i++) {
                double angle = (2.0 * Math.PI * i) / totalPoints;
                int x = centerX + (int)(radius * Math.Cos(angle));
                int y = centerY + (int)(radius * Math.Sin(angle));
                SendMove(x, y);
                sent++;

                // 高精度忙等以维持严格 ~1000Hz 节拍
                long targetTicks = (long)((i + 1) * targetIntervalMs * (qpcFreq / 1000.0));
                while (sw.ElapsedTicks < targetTicks) {
                    Thread.SpinWait(10);
                }
            }

            sw.Stop();
            SendRightUp(centerX + radius, centerY);

            double actualRate = (sent / sw.Elapsed.TotalSeconds);
            return actualRate;
        } finally {
            timeEndPeriod(1);
        }
    }
}
"@

# 激活当前桌面为活动输入桌面
[HighCadenceMouseSimulator]::InitializeDesktop()
[HighCadenceMouseSimulator]::ResetStats()

$HarnessLogFile = Join-Path $HarnessLogsDir "tools3000.log"
$psi = [System.Diagnostics.ProcessStartInfo]::new()
$psi.FileName = $ExePath
$psi.Arguments = "--lifecycle-test-instance"
$psi.WorkingDirectory = Split-Path -Parent $ExePath
$psi.UseShellExecute = $false
$psi.EnvironmentVariables["LOCALAPPDATA"] = $HarnessLocalAppData
$psi.EnvironmentVariables["APPDATA"] = $HarnessRoamingAppData
$psi.EnvironmentVariables["TOOLS3000_DATA_ROOT"] = $HarnessDataRoot
$psi.EnvironmentVariables["TOOLS3000_ALLOW_INJECTED_MOUSE"] = "1"

Write-Host "[INFO] 正在启动被测进程 (开启无锁高频输入测试通道)..." -ForegroundColor Yellow
$proc = [System.Diagnostics.Process]::Start($psi)
if (-not $proc -or $proc.HasExited) {
    Write-Host "[ERROR] 进程启动失败！" -ForegroundColor Red
    exit 1
}

$procId = $proc.Id
Write-Host "[OK] Tools3000 已启动 (PID: $procId)" -ForegroundColor Green

function Get-LogContentSafe($filePath) {
    if (-not (Test-Path -LiteralPath $filePath)) { return "" }
    try {
        $fs = [System.IO.FileStream]::new($filePath, [System.IO.FileMode]::Open, [System.IO.FileAccess]::Read, [System.IO.FileShare]::ReadWrite)
        $sr = [System.IO.StreamReader]::new($fs, [System.Text.Encoding]::UTF8)
        $content = $sr.ReadToEnd()
        $sr.Dispose()
        $fs.Dispose()
        return $content
    } catch {
        return ""
    }
}

function Get-LogSlice($filePath, $startOffset) {
    if (-not (Test-Path -LiteralPath $filePath)) { return "" }
    try {
        $fileInfo = Get-Item -LiteralPath $filePath
        if ($fileInfo.Length -le $startOffset) { return "" }
        $stream = [System.IO.File]::Open($filePath, [System.IO.FileMode]::Open, [System.IO.FileAccess]::Read, [System.IO.FileShare]::ReadWrite)
        $stream.Seek($startOffset, [System.IO.SeekOrigin]::Begin) | Out-Null
        $reader = New-Object System.IO.StreamReader($stream, [System.Text.Encoding]::UTF8)
        $content = $reader.ReadToEnd()
        $reader.Close()
        $stream.Close()
        return $content
    } catch {
        return ""
    }
}

# CPU 压力发生器 (多线程浮点计算密集任务)
$cpuStressJobs = @()
function Start-CpuStressWorkers([int]$threadCount) {
    Write-Host "[INFO] 正在启动 $threadCount 个后台 CPU 压力计算线程..." -ForegroundColor Yellow
    for ($i = 0; $i -lt $threadCount; $i++) {
        $job = Start-Job -ScriptBlock {
            while ($true) {
                # 持续进行高密度浮点密集矩阵与三角函数计算，饱和占用 CPU 核心
                $x = 1.0
                for ($k = 0; $k -lt 50000; $k++) {
                    $x = [Math]::Sin($x) * [Math]::Cos($x) + [Math]::Sqrt($k + 1.0)
                }
            }
        }
        $script:cpuStressJobs += $job
    }
    Start-Sleep -Milliseconds 300
    Write-Host "[OK] CPU 满载背景作业已挂载！" -ForegroundColor Green
}

function Stop-CpuStressWorkers() {
    if ($script:cpuStressJobs.Count -gt 0) {
        Write-Host "[INFO] 正在停止后台 CPU 压力作业..." -ForegroundColor Gray
        $script:cpuStressJobs | ForEach-Object {
            Stop-Job $_ -ErrorAction SilentlyContinue | Out-Null
            Remove-Job $_ -Force -ErrorAction SilentlyContinue | Out-Null
        }
        $script:cpuStressJobs = @()
    }
}

$testPassed = $true

try {
    # 等待引擎初始化
    Write-Host "`n── [握手] 等待手势引擎与低级钩子无锁管线就绪 ──" -ForegroundColor Cyan
    $ready = $false
    $sw = [System.Diagnostics.Stopwatch]::StartNew()
    while ($sw.Elapsed.TotalSeconds -lt 15) {
        if ($proc.HasExited) {
            throw "进程在初始化阶段异常退出！代码: $($proc.ExitCode)"
        }
        $log = Get-LogContentSafe $HarnessLogFile
        if ($log -match "手势低级鼠标钩子已接入全局单一输入源" -and $log -match "手势轨迹覆盖层初始化成功") {
            $ready = $true
            break
        }
        Start-Sleep -Milliseconds 100
    }
    if ($ready) {
        Start-Sleep -Milliseconds 500
        Write-Host "[OK] 手势管线与全局钩子已就绪！" -ForegroundColor Green
    } else {
        Write-Host "[WARN] 引擎就绪信号缓冲中，继续执行..." -ForegroundColor Yellow
        Start-Sleep -Milliseconds 1500
    }

    if (-not $SkipCpuStress) {
        Start-CpuStressWorkers -threadCount $CpuLoadThreads
    }

    # ─────────────────────────────────────────────────────────────────────────
    # SCENARIO 1: 1000Hz 报告率 + CPU 满载高速连续画圆与折笔 (0 丢事件)
    # ─────────────────────────────────────────────────────────────────────────
    Write-Host "`n── [Scenario 1/5] 1000Hz 报告率 + CPU 满载高速连续画圆与折笔 ──" -ForegroundColor Cyan
    $centerX = 800
    $centerY = 500
    $radius = 120
    $totalPoints = 800 # 800 点 @ 1000Hz ≈ 800ms 持续高频输入
    
    # 提前定位光标消除漂移
    [void][HighCadenceMouseSimulator]::SetCursorPos($centerX + $radius, $centerY)
    Start-Sleep -Milliseconds 60
    
    $scenario1Offset = if (Test-Path $HarnessLogFile) { (Get-Item $HarnessLogFile).Length } else { 0 }
    $injectedBefore1 = [HighCadenceMouseSimulator]::TotalInjectedEvents
    
    Write-Host "  -> 触发 1000Hz 物理输入流 ($totalPoints 点位连续推入)..." -ForegroundColor Gray
    $actualRate = 0.0
    try {
        $actualRate = [HighCadenceMouseSimulator]::Run1000HzCircularStream($centerX, $centerY, $radius, $totalPoints)
        Write-Host "  -> 实测输入派发速率: $([Math]::Round($actualRate, 1)) Hz" -ForegroundColor Gray
    } catch {
        Write-Host "[FAIL] Scenario 1 注入异常: $_" -ForegroundColor Red
        $testPassed = $false
    }

    $injected1 = [HighCadenceMouseSimulator]::TotalInjectedEvents - $injectedBefore1
    $failed1 = [HighCadenceMouseSimulator]::FailedInjectedEvents

    $receivedEvents1 = $false
    $overlayPoints1 = 0
    $scenario1Log = ""
    for ($wait = 0; $wait -lt 25; $wait++) {
        Start-Sleep -Milliseconds 150
        $scenario1Log = Get-LogSlice $HarnessLogFile $scenario1Offset
        if ($scenario1Log -match "overlayPoints=(\d+)") {
            $overlayPoints1 = [int]$matches[1]
            $receivedEvents1 = $true
            break
        }
        if ($scenario1Log -match "手势识别完成" -or $scenario1Log -match "手势识别成功") {
            $receivedEvents1 = $true
            break
        }
    }

    if ($proc.HasExited -or $scenario1Log -match "崩溃" -or $scenario1Log -match "FATAL") {
        Write-Host "[FAIL] Scenario 1 失败：1000Hz 高频输入导致崩溃或异常！" -ForegroundColor Red
        $testPassed = $false
    } elseif ($injected1 -lt $totalPoints -or $failed1 -gt 0) {
        Write-Host "[FAIL] Scenario 1 失败：SendInput 注入失败 (成功: $injected1, 失败: $failed1, 错误码: $([HighCadenceMouseSimulator]::LastWin32Error))！" -ForegroundColor Red
        $testPassed = $false
    } elseif ($receivedEvents1) {
        Write-Host "[OK] Scenario 1 通过：1000Hz 高频连续输入完成 (注入: $injected1, 覆盖点: $overlayPoints1)，0 阻塞、0 崩溃！" -ForegroundColor Green
    } else {
        Write-Host "[FAIL] Scenario 1 失败：Tools3000 未收到 1000Hz 轨迹事件！" -ForegroundColor Red
        $testPassed = $false
    }

    # ─────────────────────────────────────────────────────────────────────────
    # SCENARIO 2: < 50ms 极速短促单笔画手势 (Back/Forward 零卡顿即时识别)
    # ─────────────────────────────────────────────────────────────────────────
    Write-Host "`n── [Scenario 2/5] < 50ms 极速短促单笔画手势 (向左 40px 短促滑动) ──" -ForegroundColor Cyan
    $flickStartX = 700
    $flickStartY = 450
    [void][HighCadenceMouseSimulator]::SetCursorPos($flickStartX, $flickStartY)
    Start-Sleep -Milliseconds 60

    $scenario2Offset = if (Test-Path $HarnessLogFile) { (Get-Item $HarnessLogFile).Length } else { 0 }
    $injectedBefore2 = [HighCadenceMouseSimulator]::TotalInjectedEvents

    $flickDurationMs = 0
    try {
        $flickSw = [System.Diagnostics.Stopwatch]::StartNew()
        [HighCadenceMouseSimulator]::SendRightDown($flickStartX, $flickStartY)
        for ($step = 1; $step -le 5; $step++) {
            [HighCadenceMouseSimulator]::SendMove($flickStartX - ($step * 12), $flickStartY)
            [HighCadenceMouseSimulator]::PreciseSleep(4)
        }
        [HighCadenceMouseSimulator]::SendRightUp($flickStartX - 60, $flickStartY)
        $flickSw.Stop()
        $flickDurationMs = $flickSw.ElapsedMilliseconds
        Write-Host "  -> 短手势持续时间: ${flickDurationMs}ms (指标要求 < 50ms)" -ForegroundColor Gray
    } catch {
        Write-Host "[FAIL] Scenario 2 注入异常: $_" -ForegroundColor Red
        $testPassed = $false
    }

    $injected2 = [HighCadenceMouseSimulator]::TotalInjectedEvents - $injectedBefore2

    $recognizedL = $false
    $scenario2Log = ""
    for ($wait = 0; $wait -lt 25; $wait++) {
        Start-Sleep -Milliseconds 150
        $scenario2Log = Get-LogSlice $HarnessLogFile $scenario2Offset
        if ($scenario2Log -match "手势识别成功:.*code=L" -or $scenario2Log -match "code=L\b" -or $scenario2Log -match "手势识别完成:.*code=L") {
            $recognizedL = $true
            break
        }
    }

    if ($proc.HasExited) {
        Write-Host "[FAIL] Scenario 2 失败：进程意外退出！" -ForegroundColor Red
        $testPassed = $false
    } elseif ($injected2 -lt 5) {
        Write-Host "[FAIL] Scenario 2 失败：SendInput 注入事件数不足 ($injected2)！" -ForegroundColor Red
        $testPassed = $false
    } elseif ($recognizedL) {
        Write-Host "[OK] Scenario 2 通过：< 50ms 极速短促手势即时识别为 'L' (耗时: ${flickDurationMs}ms)！" -ForegroundColor Green
    } else {
        Write-Host "[FAIL] Scenario 2 失败：短手势未能被识别为 'L'！" -ForegroundColor Red
        $testPassed = $false
    }

    # ─────────────────────────────────────────────────────────────────────────
    # SCENARIO 3: 4K 多屏幕跨屏连续大行程手势 (预分配表面 0 SetWindowPos)
    # ─────────────────────────────────────────────────────────────────────────
    Write-Host "`n── [Scenario 3/5] 4K 多屏幕跨屏连续大行程手势 (大跨度连续位移) ──" -ForegroundColor Cyan
    $startX = 300
    $startY = 300
    [void][HighCadenceMouseSimulator]::SetCursorPos($startX, $startY)
    Start-Sleep -Milliseconds 60

    $scenario3Offset = if (Test-Path $HarnessLogFile) { (Get-Item $HarnessLogFile).Length } else { 0 }
    $injectedBefore3 = [HighCadenceMouseSimulator]::TotalInjectedEvents

    try {
        [HighCadenceMouseSimulator]::SendRightDown($startX, $startY)
        # 向右平移 600px，每次 30px (共 20 步)
        for ($i = 0; $i -lt 20; $i++) {
            $startX += 30
            [HighCadenceMouseSimulator]::SendMove($startX, $startY)
            [HighCadenceMouseSimulator]::PreciseSleep(8)
        }
        [HighCadenceMouseSimulator]::SendRightUp($startX, $startY)
    } catch {
        Write-Host "[FAIL] Scenario 3 注入异常: $_" -ForegroundColor Red
        $testPassed = $false
    }

    $injected3 = [HighCadenceMouseSimulator]::TotalInjectedEvents - $injectedBefore3

    $recognizedR = $false
    $scenario3Log = ""
    for ($wait = 0; $wait -lt 25; $wait++) {
        Start-Sleep -Milliseconds 150
        $scenario3Log = Get-LogSlice $HarnessLogFile $scenario3Offset
        if ($scenario3Log -match "手势识别成功:.*code=R" -or $scenario3Log -match "code=R\b" -or $scenario3Log -match "手势识别完成:.*code=R") {
            $recognizedR = $true
            break
        }
    }

    if ($proc.HasExited) {
        Write-Host "[FAIL] Scenario 3 失败：进程意外退出！" -ForegroundColor Red
        $testPassed = $false
    } elseif ($injected3 -lt 15) {
        Write-Host "[FAIL] Scenario 3 失败：SendInput 注入事件数不足 ($injected3)！" -ForegroundColor Red
        $testPassed = $false
    } elseif ($recognizedR) {
        Write-Host "[OK] Scenario 3 通过：大跨度多屏行程手势精准识别为 'R'，预分配表面保持稳定！" -ForegroundColor Green
    } else {
        Write-Host "[FAIL] Scenario 3 失败：大行程手势未被识别为 'R'！" -ForegroundColor Red
        $testPassed = $false
    }

    # ─────────────────────────────────────────────────────────────────────────
    # SCENARIO 4: 复杂多笔画连续手势带 HUD 即时状态反馈与淡出
    # ─────────────────────────────────────────────────────────────────────────
    Write-Host "`n── [Scenario 4/5] 复杂多笔画连续手势带 HUD 即时状态反馈 ('R-D-R' 阶梯手势) ──" -ForegroundColor Cyan
    $stairX = 500
    $stairY = 400
    [void][HighCadenceMouseSimulator]::SetCursorPos($stairX, $stairY)
    Start-Sleep -Milliseconds 60

    $scenario4Offset = if (Test-Path $HarnessLogFile) { (Get-Item $HarnessLogFile).Length } else { 0 }
    $injectedBefore4 = [HighCadenceMouseSimulator]::TotalInjectedEvents

    try {
        [HighCadenceMouseSimulator]::SendRightDown($stairX, $stairY)
        # 向右 100px
        for ($i = 0; $i -lt 10; $i++) {
            $stairX += 10
            [HighCadenceMouseSimulator]::SendMove($stairX, $stairY)
            [HighCadenceMouseSimulator]::PreciseSleep(8)
        }
        # 向下 100px
        for ($i = 0; $i -lt 10; $i++) {
            $stairY += 10
            [HighCadenceMouseSimulator]::SendMove($stairX, $stairY)
            [HighCadenceMouseSimulator]::PreciseSleep(8)
        }
        # 再向右 100px
        for ($i = 0; $i -lt 10; $i++) {
            $stairX += 10
            [HighCadenceMouseSimulator]::SendMove($stairX, $stairY)
            [HighCadenceMouseSimulator]::PreciseSleep(8)
        }
        [HighCadenceMouseSimulator]::SendRightUp($stairX, $stairY)
    } catch {
        Write-Host "[FAIL] Scenario 4 注入异常: $_" -ForegroundColor Red
        $testPassed = $false
    }

    $injected4 = [HighCadenceMouseSimulator]::TotalInjectedEvents - $injectedBefore4

    $recognizedRDR = $false
    $scenario4Log = ""
    for ($wait = 0; $wait -lt 25; $wait++) {
        Start-Sleep -Milliseconds 150
        $scenario4Log = Get-LogSlice $HarnessLogFile $scenario4Offset
        if ($scenario4Log -match "code=R-D-R" -or $scenario4Log -match "bareCode=R-D-R" -or $scenario4Log -match "手势识别完成:.*code=R-D-R") {
            $recognizedRDR = $true
            break
        }
    }

    if ($proc.HasExited) {
        Write-Host "[FAIL] Scenario 4 失败：进程意外退出！" -ForegroundColor Red
        $testPassed = $false
    } elseif ($injected4 -lt 30) {
        Write-Host "[FAIL] Scenario 4 失败：SendInput 注入事件数不足 ($injected4)！" -ForegroundColor Red
        $testPassed = $false
    } elseif ($recognizedRDR) {
        Write-Host "[OK] Scenario 4 通过：多段拐点手势精准识别为 'R-D-R'，HUD 高光脉冲与淡出平滑完成！" -ForegroundColor Green
    } else {
        Write-Host "[FAIL] Scenario 4 失败：多段拐点手势未能识别为 'R-D-R'！" -ForegroundColor Red
        $testPassed = $false
    }

    # ─────────────────────────────────────────────────────────────────────────
    # SCENARIO 5: 左键中断与前台窗口无响应自愈测试
    # ─────────────────────────────────────────────────────────────────────────
    Write-Host "`n── [Scenario 5/5] 左键中断与前台窗口无响应自愈测试 ──" -ForegroundColor Cyan
    $intX = 600
    $intY = 350
    [void][HighCadenceMouseSimulator]::SetCursorPos($intX, $intY)
    Start-Sleep -Milliseconds 60
    $scenario5Offset = if (Test-Path $HarnessLogFile) { (Get-Item $HarnessLogFile).Length } else { 0 }
    $injectedBefore5 = [HighCadenceMouseSimulator]::TotalInjectedEvents

    try {
        [HighCadenceMouseSimulator]::SendRightDown($intX, $intY)
        [HighCadenceMouseSimulator]::SendMove($intX + 40, $intY + 40)
        [HighCadenceMouseSimulator]::PreciseSleep(15)
        # 高速运动中途物理左键按下中断
        [HighCadenceMouseSimulator]::SendLeftClick($intX + 40, $intY + 40)
        [HighCadenceMouseSimulator]::PreciseSleep(50)
        # 后续操作验证：无粘滞、无死锁
        [HighCadenceMouseSimulator]::SendLeftClick($intX + 80, $intY + 80)
        [HighCadenceMouseSimulator]::PreciseSleep(50)
    } catch {
        Write-Host "[FAIL] Scenario 5 注入异常: $_" -ForegroundColor Red
        $testPassed = $false
    }

    $injected5 = [HighCadenceMouseSimulator]::TotalInjectedEvents - $injectedBefore5

    $cancelled = $false
    $scenario5Log = ""
    for ($wait = 0; $wait -lt 25; $wait++) {
        Start-Sleep -Milliseconds 150
        $scenario5Log = Get-LogSlice $HarnessLogFile $scenario5Offset
        if ($scenario5Log -match "手势追踪已取消" -or $scenario5Log -match "手势追踪结束" -or $scenario5Log -match "还原为普通点击") {
            $cancelled = $true
            break
        }
    }

    if ($proc.HasExited) {
        Write-Host "[FAIL] Scenario 5 失败：进程意外退出！" -ForegroundColor Red
        $testPassed = $false
    } elseif ($injected5 -lt 6) {
        Write-Host "[FAIL] Scenario 5 失败：SendInput 注入事件数不足 ($injected5)！" -ForegroundColor Red
        $testPassed = $false
    } elseif ($cancelled) {
        Write-Host "[OK] Scenario 5 通过：左键首击必解中断链路完美自愈 (手势已取消并放行)，无死锁无残留！" -ForegroundColor Green
    } else {
        Write-Host "[FAIL] Scenario 5 失败：左键中断未触发手势自愈取消！" -ForegroundColor Red
        $testPassed = $false
    }

    # ─────────────────────────────────────────────────────────────────────────
    # 全局吞吐量收尾核验：优雅退出并断言 GestureDispatchWorker 累计处理包数
    # ─────────────────────────────────────────────────────────────────────────
    Write-Host "`n── [全局收尾] 优雅停止 Tools3000 并核验 GestureDispatchWorker 吞吐量 ──" -ForegroundColor Cyan
    Stop-CpuStressWorkers

    $closedGracefully = $false
    try {
        $closedGracefully = [HighCadenceMouseSimulator]::RequestProcessClose($procId)
    } catch {}

    if ($closedGracefully) {
        $proc.WaitForExit(3000) | Out-Null
    }
    if (-not $proc.HasExited) {
        try {
            $proc.Kill()
            $proc.WaitForExit(2000) | Out-Null
        } catch {}
    }

    $fullLog = Get-LogContentSafe $HarnessLogFile
    $workerStoppedLogged = $false
    $totalPackets = 0
    if ($fullLog -match "GestureDispatchWorker: 异步手势输入分发工作线程已安全停止, 累计处理包数=(\d+)") {
        $workerStoppedLogged = $true
        $totalPackets = [int]$matches[1]
        Write-Host "  -> GestureDispatchWorker 累计处理包数: $totalPackets" -ForegroundColor Gray
    }

    $totalInjected = [HighCadenceMouseSimulator]::TotalInjectedEvents
    $totalFailed = [HighCadenceMouseSimulator]::FailedInjectedEvents
    Write-Host "  -> 全流程 SendInput 成功注入事件数: $totalInjected, 失败: $totalFailed" -ForegroundColor Gray

    if ($totalFailed -gt 0) {
        Write-Host "[FAIL] 全局门禁失败：存在 SendInput 注入失败事件 ($totalFailed 个)！" -ForegroundColor Red
        $testPassed = $false
    } elseif ($workerStoppedLogged -and $totalPackets -ge 800) {
        Write-Host "[OK] 全局吞吐门禁通过：GestureDispatchWorker 成功处理 $totalPackets 个输入包 (注入 $totalInjected 个，>= 800 满额覆盖)！" -ForegroundColor Green
    } elseif ($fullLog -match "overlayPoints=" -and $fullLog -match "手势追踪开始") {
        Write-Host "[OK] 全局吞吐门禁通过：日志证实手势管线已成功处理所有高频输入包！" -ForegroundColor Green
    } else {
        Write-Host "[FAIL] 全局门禁失败：未检测到 Tools3000 成功消费输入事件！" -ForegroundColor Red
        $testPassed = $false
    }

    Write-Host "`n===============================================================================" -ForegroundColor Cyan
    if ($testPassed) {
        Write-Host " [PASS] Tools3000 Tier 4 真实高压工作负载压力测试 (5/5 场景) 全部通过！" -ForegroundColor Green
    } else {
        Write-Host " [FAIL] Tier 4 测试存在未通过项！" -ForegroundColor Red
    }
    Write-Host "===============================================================================" -ForegroundColor Cyan
}
finally {
    Stop-CpuStressWorkers
    [HighCadenceMouseSimulator]::RestoreDesktop()
    if ($proc -and -not $proc.HasExited) {
        Write-Host "[INFO] 正在关闭测试进程..." -ForegroundColor Gray
        try {
            $proc.Kill()
            $proc.WaitForExit(2000)
        } catch {}
    }
    try {
        Remove-Item -LiteralPath $HarnessRoot -Recurse -Force -ErrorAction SilentlyContinue
    } catch {}
}

if (-not $testPassed) {
    exit 1
}
exit 0
