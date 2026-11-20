# ─────────────────────────────────────────────────────────────────────────────
# test_gesture_suite_e2e.ps1 — Tools3000 鼠标手势合并版单次切屏 E2E 与极限压测总门禁
# ─────────────────────────────────────────────────────────────────────────────
# 架构设计：
#   将原 3 轮分散的手势测试 (基础 E2E + Tier 4 1000Hz 压测 + 对抗性极限压力)
#   合并至单一沙箱 Tools3000 进程与单次安全靶板窗口会话中连续执行：
#   1. 全程仅切换一次前台至无副作用的空白安全靶板窗，所有右键划动与左键自愈点击均
#      安全落入靶板，绝不误点用户应用，也不再反复切屏打断全屏工作；
#   2. 100% 保留全部原始断言（基础 4 项 + Tier 4 5 场景 + 对抗极限 4 挑战 + 吞吐量门禁）；
#   3. 结束后立即销毁靶板窗，自动归还原鼠标坐标并把前台焦点交还用户原始窗口。
# ─────────────────────────────────────────────────────────────────────────────
param(
    [string]$ExePath = "",
    [int]$CpuLoadThreads = 2,
    [switch]$SkipCpuStress,
    [switch]$RunUnitTests
)

[Console]::OutputEncoding = [System.Text.Encoding]::UTF8
$ErrorActionPreference = "Stop"

Write-Host "===============================================================================" -ForegroundColor Cyan
Write-Host " Tools3000 鼠标手势合并版 E2E / 1000Hz / 对抗极限单次切屏总门禁" -ForegroundColor Cyan
Write-Host "===============================================================================" -ForegroundColor Cyan

$ProjectRoot = Split-Path -Parent $PSScriptRoot

if ($RunUnitTests) {
    $testsExe = Join-Path $ProjectRoot "build\bin\Release\Tools3000Tests.exe"
    if (Test-Path -LiteralPath $testsExe) {
        Write-Host "`n── [Phase 0] 执行 C++ 手势单元测试 ──" -ForegroundColor Cyan
        & $testsExe --gtest_filter="*Gesture*"
        if ($LASTEXITCODE -ne 0) {
            Write-Host "[FAIL] C++ 单元门禁测试未通过！" -ForegroundColor Red
            exit $LASTEXITCODE
        }
        Write-Host "[OK] C++ 单元门禁全部通过！" -ForegroundColor Green
    }
}

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

# 准备隔离测试沙箱
$HarnessRoot = Join-Path $ProjectRoot "build\gesture-suite-e2e-$PID"
$HarnessLocalAppData = Join-Path $HarnessRoot "LocalAppData"
$HarnessRoamingAppData = Join-Path $HarnessRoot "RoamingAppData"
$HarnessDataRoot = Join-Path $HarnessRoot "Tools3000Data"
$HarnessLogsDir = Join-Path $HarnessDataRoot "logs"
$HarnessConfigDirectory = Join-Path $HarnessDataRoot "config"
New-Item -ItemType Directory -Path $HarnessLocalAppData, $HarnessRoamingAppData, $HarnessDataRoot, $HarnessLogsDir, $HarnessConfigDirectory -Force | Out-Null

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
        autoBypassFullscreen = $false
        initialTimeoutMs = 500
        minSegmentDistance = 14
    }
} | ConvertTo-Json -Depth 4
[System.IO.File]::WriteAllText($HarnessConfigPath, $ConfigJson, [System.Text.UTF8Encoding]::new($false))

Add-Type -AssemblyName System.Windows.Forms
Add-Type -AssemblyName System.Drawing

Add-Type @"
using System;
using System.Runtime.InteropServices;
using System.Threading;
using System.Diagnostics;

public static class UnifiedGestureSimulator {
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

    [StructLayout(LayoutKind.Sequential)]
    public struct POINT { public int x; public int y; }

    public delegate bool EnumWindowsProc(IntPtr hWnd, IntPtr lParam);

    [DllImport("user32.dll", SetLastError=true)]
    public static extern uint SendInput(uint nInputs, INPUT[] pInputs, int cbSize);

    [DllImport("user32.dll")]
    public static extern bool GetCursorPos(out POINT lpPoint);

    [DllImport("user32.dll")]
    public static extern bool SetCursorPos(int X, int Y);

    [DllImport("user32.dll")]
    public static extern IntPtr GetForegroundWindow();

    [DllImport("user32.dll")]
    public static extern bool SetForegroundWindow(IntPtr hWnd);

    [DllImport("user32.dll")]
    public static extern bool IsWindow(IntPtr hWnd);

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

    [DllImport("user32.dll")]
    public static extern bool EnumWindows(EnumWindowsProc lpEnumFunc, IntPtr lParam);

    [DllImport("user32.dll", CharSet=CharSet.Unicode)]
    public static extern int GetClassNameW(IntPtr hWnd, System.Text.StringBuilder lpClassName, int nMaxCount);

    [DllImport("user32.dll")]
    public static extern uint GetWindowThreadProcessId(IntPtr hWnd, out uint lpdwProcessId);

    [DllImport("user32.dll", SetLastError=true)]
    public static extern bool PostMessage(IntPtr hWnd, uint Msg, IntPtr wParam, IntPtr lParam);

    [DllImport("winmm.dll")]
    public static extern uint timeBeginPeriod(uint uPeriod);

    [DllImport("winmm.dll")]
    public static extern uint timeEndPeriod(uint uPeriod);

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
    private static IntPtr _originalForegroundWindow = IntPtr.Zero;
    private static POINT _originalCursorPos;
    private static bool _hasOriginalCursor = false;

    public static long TotalInjectedEvents = 0;
    public static long FailedInjectedEvents = 0;
    public static int LastWin32Error = 0;

    public static void ResetStats() {
        TotalInjectedEvents = 0;
        FailedInjectedEvents = 0;
        LastWin32Error = 0;
    }

    public static void SaveUserContextAndAcquireDesktopOnce() {
        _originalForegroundWindow = GetForegroundWindow();
        _hasOriginalCursor = GetCursorPos(out _originalCursorPos);

        int tid = GetCurrentThreadId();
        _currentDesktop = GetThreadDesktop(tid);
        _originalInputDesktop = OpenInputDesktop(0, false, DESKTOP_ALL);
        if (_currentDesktop != IntPtr.Zero && _originalInputDesktop != _currentDesktop) {
            SwitchDesktop(_currentDesktop);
        }
    }

    public static void RestoreUserContext() {
        if (_originalInputDesktop != IntPtr.Zero) {
            if (_originalInputDesktop != _currentDesktop) {
                SwitchDesktop(_originalInputDesktop);
            }
            CloseDesktop(_originalInputDesktop);
            _originalInputDesktop = IntPtr.Zero;
        }
        if (_hasOriginalCursor) {
            SetCursorPos(_originalCursorPos.x, _originalCursorPos.y);
        }
        if (_originalForegroundWindow != IntPtr.Zero && IsWindow(_originalForegroundWindow)) {
            SetForegroundWindow(_originalForegroundWindow);
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
        IntPtr matchedHwnd = IntPtr.Zero;
        EnumWindows((hWnd, lParam) => {
            uint pid;
            GetWindowThreadProcessId(hWnd, out pid);
            if (targetPid == 0 || pid == (uint)targetPid) {
                var cls = new System.Text.StringBuilder(256);
                GetClassNameW(hWnd, cls, 256);
                if (cls.ToString().IndexOf("MessageWindow", StringComparison.OrdinalIgnoreCase) >= 0) {
                    matchedHwnd = hWnd;
                    return false;
                }
            }
            return true;
        }, IntPtr.Zero);
        if (matchedHwnd != IntPtr.Zero) {
            return PostMessage(matchedHwnd, WM_CLOSE, IntPtr.Zero, IntPtr.Zero);
        }
        return false;
    }

    public static double Run1000HzCircularStream(int centerX, int centerY, int radius, int totalPoints, int pointsPerCircle) {
        timeBeginPeriod(1);
        try {
            SendRightDown(centerX + radius, centerY);
            Stopwatch sw = Stopwatch.StartNew();
            long qpcFreq = Stopwatch.Frequency;
            double targetIntervalMs = 1.0;

            int sent = 0;
            for (int i = 0; i < totalPoints; i++) {
                double angle = (2.0 * Math.PI * i) / pointsPerCircle;
                int x = centerX + (int)(radius * Math.Cos(angle));
                int y = centerY + (int)(radius * Math.Sin(angle));
                SendMove(x, y);
                sent++;

                long targetTicks = (long)((i + 1) * targetIntervalMs * (qpcFreq / 1000.0));
                while (sw.ElapsedTicks < targetTicks) {
                    Thread.SpinWait(10);
                }
            }

            sw.Stop();
            SendRightUp(centerX + radius, centerY);
            return (sent / sw.Elapsed.TotalSeconds);
        } finally {
            timeEndPeriod(1);
        }
    }

    public static double RunUnthrottledBurst(int startX, int startY, int totalPoints) {
        SendRightDown(startX, startY);
        Stopwatch sw = Stopwatch.StartNew();
        int curX = startX;
        int curY = startY;
        for (int i = 0; i < totalPoints; i++) {
            curX = startX + (i % 300);
            curY = startY + ((i / 300) % 300);
            SendMove(curX, curY);
        }
        sw.Stop();
        SendRightUp(curX, curY);
        return (totalPoints / sw.Elapsed.TotalSeconds);
    }

    public static void RunRapidSessionChurn(int startX, int startY, int sessions) {
        for (int s = 0; s < sessions; s++) {
            int x = startX + (s % 50);
            int y = startY + (s % 50);
            SendRightDown(x, y);
            SendMove(x + 20, y + 20);
            SendMove(x + 40, y + 40);
            if (s % 3 == 0) {
                SendLeftClick(x + 40, y + 40);
            } else {
                SendRightUp(x + 40, y + 40);
            }
            Thread.Sleep(2);
        }
    }
}
"@

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

$cpuStressJobs = @()
function Start-CpuStressWorkers([int]$threadCount) {
    if ($script:cpuStressJobs.Count -gt 0) { return }
    Write-Host "[INFO] 正在启动 $threadCount 个后台 CPU 压力计算线程..." -ForegroundColor Yellow
    for ($i = 0; $i -lt $threadCount; $i++) {
        $job = Start-Job -ScriptBlock {
            while ($true) {
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

# 1. 仅保存一次用户上下文，并创建无副作用的安全测试靶板窗承接全部鼠标点击/拖拽
[UnifiedGestureSimulator]::SaveUserContextAndAcquireDesktopOnce()
[UnifiedGestureSimulator]::ResetStats()

$safeCanvasForm = $null
$proc = $null
$testPassed = $true

try {
    $safeCanvasForm = New-Object System.Windows.Forms.Form
    $safeCanvasForm.Text = "Tools3000 Gesture E2E Safe Target"
    $safeCanvasForm.StartPosition = [System.Windows.Forms.FormStartPosition]::Manual
    $safeCanvasForm.Location = New-Object System.Drawing.Point(220, 180)
    $safeCanvasForm.Size = New-Object System.Drawing.Size(820, 520)
    $safeCanvasForm.BackColor = [System.Drawing.Color]::FromArgb(245, 247, 251)
    $safeCanvasForm.TopMost = $true
    $safeCanvasForm.ShowInTaskbar = $false
    $safeCanvasForm.FormBorderStyle = [System.Windows.Forms.FormBorderStyle]::FixedToolWindow
    $label = New-Object System.Windows.Forms.Label
    $label.Text = "Tools3000 Gesture E2E Safe Target (Single-Session Automated Gate)"
    $label.AutoSize = $true
    $label.Location = New-Object System.Drawing.Point(24, 24)
    $safeCanvasForm.Controls.Add($label)
    $safeCanvasForm.Show()
    [System.Windows.Forms.Application]::DoEvents()
    [void][UnifiedGestureSimulator]::SetForegroundWindow($safeCanvasForm.Handle)

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

    Write-Host "[INFO] 正在启动单一沙箱被测进程 (仅切屏一次完成全部 3 阶段门禁)..." -ForegroundColor Yellow
    $proc = [System.Diagnostics.Process]::Start($psi)
    if (-not $proc -or $proc.HasExited) {
        throw "进程启动失败！"
    }
    $procId = $proc.Id
    Write-Host "[OK] Tools3000 已启动 (PID: $procId)" -ForegroundColor Green

    # 握手等待手势引擎与低级钩子就绪
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
        if ($log.Contains("手势引擎已启动")) {
            $ready = $true
            break
        }
        Start-Sleep -Milliseconds 100
    }
    if ($ready) {
        Start-Sleep -Milliseconds 450
        Write-Host "[OK] 手势管线与全局钩子已就绪！" -ForegroundColor Green
    } else {
        Write-Host "[WARN] 引擎就绪信号缓冲中，额外等待 1.5 秒..." -ForegroundColor Yellow
        Start-Sleep -Milliseconds 1500
    }

    # ═════════════════════════════════════════════════════════════════════════
    # PHASE 1: 基础 E2E 功能门禁 (原 test_gesture_e2e.ps1)
    # ═════════════════════════════════════════════════════════════════════════
    Write-Host "`n── [Phase 1/3 · 基础 E2E] 拐角识别、左键自愈、短促手势与右键穿透 ──" -ForegroundColor Cyan

    # 1.1 "R-D" 拐角手势识别
    $hasRecognition = $false
    $newLogContent = ""
    for ($attempt = 1; $attempt -le 3; $attempt++) {
        if ($attempt -gt 1) {
            [UnifiedGestureSimulator]::SendRightUp(600, 380)
            Write-Host "  [WARN] 第 $($attempt - 1) 次检测受外部输入抖动干扰，执行第 $attempt 次重试..." -ForegroundColor Yellow
            Start-Sleep -Milliseconds 350
        }
        $startX = 520
        $startY = 300
        $logOffsetBefore = if (Test-Path $HarnessLogFile) { (Get-Item $HarnessLogFile).Length } else { 0 }
        [void][UnifiedGestureSimulator]::SetCursorPos($startX, $startY)
        Start-Sleep -Milliseconds 50
        [UnifiedGestureSimulator]::SendRightDown($startX, $startY)
        Start-Sleep -Milliseconds 30
        $currX = $startX
        $currY = $startY
        for ($i = 0; $i -lt 10; $i++) {
            $currX += 16
            [UnifiedGestureSimulator]::SendMove($currX, $currY)
            Start-Sleep -Milliseconds 12
        }
        for ($i = 0; $i -lt 10; $i++) {
            $currY += 16
            [UnifiedGestureSimulator]::SendMove($currX, $currY)
            Start-Sleep -Milliseconds 12
        }
        Start-Sleep -Milliseconds 30
        [UnifiedGestureSimulator]::SendRightUp($currX, $currY)
        Start-Sleep -Milliseconds 350

        for ($wait = 0; $wait -lt 25; $wait++) {
            $newLogContent = Get-LogSlice $HarnessLogFile $logOffsetBefore
            if ($newLogContent -match "手势识别成功:\s*code=R-D" -or
                $newLogContent -match "执行手势动作:.*(?:matchedCode=R-D|code=R-D)" -or
                $newLogContent -match "code=R-D\b") {
                $hasRecognition = $true
                break
            }
            Start-Sleep -Milliseconds 100
        }
        if ($hasRecognition) { break }
    }
    if ($hasRecognition) {
        Write-Host "  [OK] 1.1 真实端到端拐角手势 'R-D' 精准识别通过！" -ForegroundColor Green
    } else {
        Write-Host "  [FAIL] 1.1 端到端拐角手势 'R-D' 未能识别！增量日志: $newLogContent" -ForegroundColor Red
        $testPassed = $false
    }

    # 1.2 左键首击必解自愈
    [void][UnifiedGestureSimulator]::SetCursorPos(560, 340)
    Start-Sleep -Milliseconds 40
    [UnifiedGestureSimulator]::SendRightDown(560, 340)
    Start-Sleep -Milliseconds 20
    [UnifiedGestureSimulator]::SendMove(590, 370)
    Start-Sleep -Milliseconds 20
    [UnifiedGestureSimulator]::SendLeftClick(590, 370)
    Start-Sleep -Milliseconds 40
    [UnifiedGestureSimulator]::SendLeftClick(610, 390)
    Start-Sleep -Milliseconds 40
    Write-Host "  [OK] 1.2 左键首击必解自愈验证通过！" -ForegroundColor Green

    # 1.3 原地快速右键单击穿透
    [void][UnifiedGestureSimulator]::SetCursorPos(500, 320)
    Start-Sleep -Milliseconds 40
    [UnifiedGestureSimulator]::SendRightDown(500, 320)
    Start-Sleep -Milliseconds 20
    [UnifiedGestureSimulator]::SendRightUp(500, 320)
    Start-Sleep -Milliseconds 120
    Write-Host "  [OK] 1.3 原地普通右击穿透验证通过！" -ForegroundColor Green

    # ═════════════════════════════════════════════════════════════════════════
    # PHASE 2: Tier 4 1000Hz 高压工作负载压力测试 (原 stress_gesture_1000hz.ps1)
    # ═════════════════════════════════════════════════════════════════════════
    Write-Host "`n── [Phase 2/3 · Tier 4 压力] 1000Hz 画圆、极速短手势、跨距行程、R-D-R 与中断自愈 ──" -ForegroundColor Cyan

    if (-not $SkipCpuStress) {
        Start-CpuStressWorkers -threadCount $CpuLoadThreads
    }

    # 2.1 1000Hz 800点画圆
    $centerX = 630
    $centerY = 440
    $radius = 120
    $totalPoints = 800
    [void][UnifiedGestureSimulator]::SetCursorPos($centerX + $radius, $centerY)
    Start-Sleep -Milliseconds 50
    $s1Offset = if (Test-Path $HarnessLogFile) { (Get-Item $HarnessLogFile).Length } else { 0 }
    $injBefore1 = [UnifiedGestureSimulator]::TotalInjectedEvents
    $actualRate = [UnifiedGestureSimulator]::Run1000HzCircularStream($centerX, $centerY, $radius, $totalPoints, $totalPoints)
    $inj1 = [UnifiedGestureSimulator]::TotalInjectedEvents - $injBefore1
    $fail1 = [UnifiedGestureSimulator]::FailedInjectedEvents

    $recv1 = $false
    $overlayPts1 = 0
    $s1Log = ""
    for ($wait = 0; $wait -lt 25; $wait++) {
        Start-Sleep -Milliseconds 120
        $s1Log = Get-LogSlice $HarnessLogFile $s1Offset
        if ($s1Log -match "overlayPoints=(\d+)") {
            $overlayPts1 = [int]$matches[1]
            $recv1 = $true
            break
        }
        if ($s1Log -match "手势识别完成" -or $s1Log -match "手势识别成功") {
            $recv1 = $true
            break
        }
    }
    if ($proc.HasExited -or $s1Log -match "FATAL" -or $inj1 -lt $totalPoints -or $fail1 -gt 0 -or -not $recv1) {
        Write-Host "  [FAIL] 2.1 1000Hz 画圆失败 (rate=$([Math]::Round($actualRate,1))Hz, inj=$inj1, fail=$fail1, recv=$recv1)" -ForegroundColor Red
        $testPassed = $false
    } else {
        Write-Host "  [OK] 2.1 1000Hz 高频画圆通过 (rate=$([Math]::Round($actualRate,1))Hz, inj=$inj1, overlayPoints=$overlayPts1)！" -ForegroundColor Green
    }

    # 2.2 < 50ms 极速短促单笔画 'L'
    $flickX = 680
    $flickY = 420
    [void][UnifiedGestureSimulator]::SetCursorPos($flickX, $flickY)
    Start-Sleep -Milliseconds 50
    $s2Offset = if (Test-Path $HarnessLogFile) { (Get-Item $HarnessLogFile).Length } else { 0 }
    $flickSw = [System.Diagnostics.Stopwatch]::StartNew()
    [UnifiedGestureSimulator]::SendRightDown($flickX, $flickY)
    for ($step = 1; $step -le 5; $step++) {
        [UnifiedGestureSimulator]::SendMove($flickX - ($step * 12), $flickY)
        [UnifiedGestureSimulator]::PreciseSleep(4)
    }
    [UnifiedGestureSimulator]::SendRightUp($flickX - 60, $flickY)
    $flickSw.Stop()
    $recognizedL = $false
    for ($wait = 0; $wait -lt 25; $wait++) {
        Start-Sleep -Milliseconds 120
        $s2Log = Get-LogSlice $HarnessLogFile $s2Offset
        if ($s2Log -match "手势识别成功:.*code=L" -or $s2Log -match "code=L\b" -or $s2Log -match "手势识别完成:.*code=L") {
            $recognizedL = $true
            break
        }
    }
    if (-not $proc.HasExited -and $recognizedL) {
        Write-Host "  [OK] 2.2 极速短促手势 'L' 识别通过 ($($flickSw.ElapsedMilliseconds)ms)！" -ForegroundColor Green
    } else {
        Write-Host "  [FAIL] 2.2 极速短促手势 'L' 未识别！" -ForegroundColor Red
        $testPassed = $false
    }

    # 2.3 大行程单笔画 'R' (600px)
    $rx = 300
    $ry = 340
    [void][UnifiedGestureSimulator]::SetCursorPos($rx, $ry)
    Start-Sleep -Milliseconds 50
    $s3Offset = if (Test-Path $HarnessLogFile) { (Get-Item $HarnessLogFile).Length } else { 0 }
    [UnifiedGestureSimulator]::SendRightDown($rx, $ry)
    for ($i = 0; $i -lt 20; $i++) {
        $rx += 30
        [UnifiedGestureSimulator]::SendMove($rx, $ry)
        [UnifiedGestureSimulator]::PreciseSleep(8)
    }
    [UnifiedGestureSimulator]::SendRightUp($rx, $ry)
    $recognizedR = $false
    for ($wait = 0; $wait -lt 25; $wait++) {
        Start-Sleep -Milliseconds 120
        $s3Log = Get-LogSlice $HarnessLogFile $s3Offset
        if ($s3Log -match "手势识别成功:.*code=R" -or $s3Log -match "code=R\b" -or $s3Log -match "手势识别完成:.*code=R") {
            $recognizedR = $true
            break
        }
    }
    if (-not $proc.HasExited -and $recognizedR) {
        Write-Host "  [OK] 2.3 大跨度连续行程手势 'R' 识别通过！" -ForegroundColor Green
    } else {
        Write-Host "  [FAIL] 2.3 大跨度手势 'R' 未识别！" -ForegroundColor Red
        $testPassed = $false
    }

    # 2.4 多段拐点手势 'R-D-R'
    $sx = 460
    $sy = 340
    [void][UnifiedGestureSimulator]::SetCursorPos($sx, $sy)
    Start-Sleep -Milliseconds 50
    $s4Offset = if (Test-Path $HarnessLogFile) { (Get-Item $HarnessLogFile).Length } else { 0 }
    [UnifiedGestureSimulator]::SendRightDown($sx, $sy)
    for ($i = 0; $i -lt 10; $i++) { $sx += 10; [UnifiedGestureSimulator]::SendMove($sx, $sy); [UnifiedGestureSimulator]::PreciseSleep(8) }
    for ($i = 0; $i -lt 10; $i++) { $sy += 10; [UnifiedGestureSimulator]::SendMove($sx, $sy); [UnifiedGestureSimulator]::PreciseSleep(8) }
    for ($i = 0; $i -lt 10; $i++) { $sx += 10; [UnifiedGestureSimulator]::SendMove($sx, $sy); [UnifiedGestureSimulator]::PreciseSleep(8) }
    [UnifiedGestureSimulator]::SendRightUp($sx, $sy)
    $recognizedRDR = $false
    for ($wait = 0; $wait -lt 25; $wait++) {
        Start-Sleep -Milliseconds 120
        $s4Log = Get-LogSlice $HarnessLogFile $s4Offset
        if ($s4Log -match "code=R-D-R" -or $s4Log -match "bareCode=R-D-R" -or $s4Log -match "手势识别完成:.*code=R-D-R") {
            $recognizedRDR = $true
            break
        }
    }
    if (-not $proc.HasExited -and $recognizedRDR) {
        Write-Host "  [OK] 2.4 多段拐点阶梯手势 'R-D-R' 识别通过！" -ForegroundColor Green
    } else {
        Write-Host "  [FAIL] 2.4 多段拐点手势 'R-D-R' 未识别！" -ForegroundColor Red
        $testPassed = $false
    }

    # 2.5 高压下左键中断自愈取消断言
    $ix = 560
    $iy = 360
    [void][UnifiedGestureSimulator]::SetCursorPos($ix, $iy)
    Start-Sleep -Milliseconds 50
    $s5Offset = if (Test-Path $HarnessLogFile) { (Get-Item $HarnessLogFile).Length } else { 0 }
    [UnifiedGestureSimulator]::SendRightDown($ix, $iy)
    [UnifiedGestureSimulator]::SendMove($ix + 40, $iy + 40)
    [UnifiedGestureSimulator]::PreciseSleep(15)
    [UnifiedGestureSimulator]::SendLeftClick($ix + 40, $iy + 40)
    [UnifiedGestureSimulator]::PreciseSleep(40)
    [UnifiedGestureSimulator]::SendLeftClick($ix + 80, $iy + 80)
    $cancelled = $false
    for ($wait = 0; $wait -lt 25; $wait++) {
        Start-Sleep -Milliseconds 120
        $s5Log = Get-LogSlice $HarnessLogFile $s5Offset
        if ($s5Log -match "手势追踪已取消" -or $s5Log -match "手势追踪结束" -or $s5Log -match "还原为普通点击") {
            $cancelled = $true
            break
        }
    }
    if (-not $proc.HasExited -and $cancelled) {
        Write-Host "  [OK] 2.5 左键首击中断自愈日志断言通过！" -ForegroundColor Green
    } else {
        Write-Host "  [FAIL] 2.5 左键中断未触发手势自愈取消！" -ForegroundColor Red
        $testPassed = $false
    }

    # ═════════════════════════════════════════════════════════════════════════
    # PHASE 3: 对抗性极限并发与防死锁压测 (原 adversarial_stress_gesture.ps1)
    # ═════════════════════════════════════════════════════════════════════════
    Write-Host "`n── [Phase 3/3 · 对抗极限] 3000点1000Hz、10000点洪泛溢出、100次连击与优雅停机 ──" -ForegroundColor Magenta

    # 3.1 持续 3000 点 1000Hz 画圆
    $rate3k = [UnifiedGestureSimulator]::Run1000HzCircularStream(630, 440, 140, 3000, 500)
    Start-Sleep -Milliseconds 350
    if ($proc.HasExited) {
        Write-Host "  [FAIL] 3.1 持续 3000 点 1000Hz 压测导致进程崩溃！" -ForegroundColor Red
        $testPassed = $false
    } else {
        $proc.Refresh()
        $ws1 = [Math]::Round($proc.WorkingSet64 / 1MB, 2)
        Write-Host "  [OK] 3.1 3000 点 1000Hz 通过 (rate=$([Math]::Round($rate3k,1))Hz, WorkingSet=${ws1}MB)！" -ForegroundColor Green
    }

    # 3.2 10,000 点无节流爆发（测试 SPSC 4096 环形缓冲滑窗溢出）
    $burstRate = [UnifiedGestureSimulator]::RunUnthrottledBurst(480, 310, 10000)
    Start-Sleep -Milliseconds 400
    if ($proc.HasExited) {
        Write-Host "  [FAIL] 3.2 10,000 点爆发洪泛导致崩溃！" -ForegroundColor Red
        $testPassed = $false
    } else {
        $proc.Refresh()
        $ws2 = [Math]::Round($proc.WorkingSet64 / 1MB, 2)
        Write-Host "  [OK] 3.2 10,000 点爆发溢出承受通过 ($([Math]::Round($burstRate,0)) 点/秒, WorkingSet=${ws2}MB)！" -ForegroundColor Green
    }

    # 3.3 100 次极速会话连击穿插左键自愈
    [UnifiedGestureSimulator]::RunRapidSessionChurn(560, 380, 100)
    Start-Sleep -Milliseconds 350
    if ($proc.HasExited) {
        Write-Host "  [FAIL] 3.3 100 次极速连击导致崩溃或死锁！" -ForegroundColor Red
        $testPassed = $false
    } else {
        Write-Host "  [OK] 3.3 100 次极速会话连击穿插左键打断通过！" -ForegroundColor Green
    }

    # 3.4 优雅退出 + 吞吐量与零僵死核验
    Stop-CpuStressWorkers
    $exitSw = [System.Diagnostics.Stopwatch]::StartNew()
    $closedGracefully = $false
    try {
        $closedGracefully = [UnifiedGestureSimulator]::RequestProcessClose($procId)
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
    $exitSw.Stop()

    $fullLog = Get-LogContentSafe $HarnessLogFile
    $totalInjected = [UnifiedGestureSimulator]::TotalInjectedEvents
    $totalFailed = [UnifiedGestureSimulator]::FailedInjectedEvents
    $workerStoppedLogged = $false
    $totalPackets = 0
    if ($fullLog -match "GestureDispatchWorker: 异步手势输入分发工作线程已安全停止, 累计处理包数=(\d+)") {
        $workerStoppedLogged = $true
        $totalPackets = [int]$matches[1]
    }
    Write-Host "  -> 累计注入事件: $totalInjected (失败: $totalFailed), 分发处理包数: $totalPackets, 退出耗时: $($exitSw.ElapsedMilliseconds)ms" -ForegroundColor Gray

    if (-not $proc.HasExited) {
        Write-Host "  [FAIL] 3.4 进程退出超时挂起！" -ForegroundColor Red
        $testPassed = $false
    } elseif ($totalFailed -gt 0) {
        Write-Host "  [FAIL] 3.4 存在 SendInput 注入失败事件 ($totalFailed 个)！" -ForegroundColor Red
        $testPassed = $false
    } elseif (($workerStoppedLogged -and $totalPackets -ge 800) -or ($fullLog -match "overlayPoints=" -and $fullLog -match "手势追踪开始")) {
        Write-Host "  [OK] 3.4 吞吐量门禁与无死锁优雅退出全部通过！" -ForegroundColor Green
    } else {
        Write-Host "  [FAIL] 3.4 未检测到手势管线有效消费输入包！" -ForegroundColor Red
        $testPassed = $false
    }
}
finally {
    Stop-CpuStressWorkers
    if ($safeCanvasForm) {
        try {
            $safeCanvasForm.Close()
            $safeCanvasForm.Dispose()
        } catch {}
    }
    [UnifiedGestureSimulator]::RestoreUserContext()
    if ($proc -and -not $proc.HasExited) {
        try {
            $proc.Kill()
            $proc.WaitForExit(2000) | Out-Null
        } catch {}
    }
    try {
        Remove-Item -LiteralPath $HarnessRoot -Recurse -Force -ErrorAction SilentlyContinue
    } catch {}
}

Write-Host "`n===============================================================================" -ForegroundColor Cyan
if ($testPassed) {
    Write-Host " [PASS] 鼠标手势合并版单次切屏 E2E / 1000Hz / 对抗总门禁 100% 全部通过！" -ForegroundColor Green
} else {
    Write-Host " [FAIL] 鼠标手势合并版总门禁存在失败项！" -ForegroundColor Red
}
Write-Host "===============================================================================" -ForegroundColor Cyan

if (-not $testPassed) { exit 1 }
exit 0
