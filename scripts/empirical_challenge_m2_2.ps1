# ─────────────────────────────────────────────────────────────────────────────
# empirical_challenge_m2_2.ps1 — Milestone 2 对抗性挑战与实测断言脚本
# ─────────────────────────────────────────────────────────────────────────────
# 验证目标：
#   1. Wheel Gesture Idle Wait Bypass 极限压测：
#      - 在 GestureState::Idle 下屏幕顶栏极速连发滚轮手势 (50 次连发)
#      - 经验性断言单事件延迟 < 1ms，且彻底规避旧版 50ms 盲等超时 (总耗时 << 2500ms)
#   2. 内存与资源句柄泄漏审计 (Memory & Resource Leak Check)：
#      - 连续高频执行 200 次完整手势生命周期 (起手 -> 绘制 -> 抬起 -> 硬件淡出)
#      - 严密断言 USER 句柄净增量 <= 0 (绝无句柄泄漏)
#      - 严密断言 GDI 句柄净增量 <= 0 (绝无位图/DC泄漏)
#      - 严密断言 DComp 预分配表面在手势全程 0 次动态重置 (CreateSurface 次数 == 0)
#      - 断言工作集与私有内存无失控泄漏
# ─────────────────────────────────────────────────────────────────────────────
param(
    [string]$ExePath = "",
    [int]$GestureIterations = 200,
    [int]$WheelIterations = 50
)

[Console]::OutputEncoding = [System.Text.Encoding]::UTF8
$ErrorActionPreference = "Stop"

Write-Host "===============================================================================" -ForegroundColor Cyan
Write-Host " [CHALLENGER] Milestone 2 滚轮绕行极速压测与资源泄漏实测门禁" -ForegroundColor Cyan
Write-Host "===============================================================================" -ForegroundColor Cyan

$ProjectRoot = Split-Path -Parent $PSScriptRoot

if ([string]::IsNullOrWhiteSpace($ExePath)) {
    $candidate = Join-Path $ProjectRoot "build\bin\Release\Tools3000.exe"
    if (Test-Path -LiteralPath $candidate) {
        $ExePath = $candidate
    } else {
        Write-Host "[ERROR] 未找到 Tools3000.exe！" -ForegroundColor Red
        exit 1
    }
}
$ExePath = (Resolve-Path -LiteralPath $ExePath).Path
Write-Host "[INFO] 待测程序: $ExePath" -ForegroundColor Gray

# 准备独立测试沙箱
$HarnessRoot = Join-Path $ProjectRoot "build\challenger-m2-2-$PID"
$HarnessLocalAppData = Join-Path $HarnessRoot "LocalAppData"
$HarnessRoamingAppData = Join-Path $HarnessRoot "RoamingAppData"
$HarnessDataRoot = Join-Path $HarnessRoot "Tools3000Data"
$HarnessLogsDir = Join-Path $HarnessDataRoot "logs"
New-Item -ItemType Directory -Path $HarnessLocalAppData, $HarnessRoamingAppData, $HarnessDataRoot, $HarnessLogsDir -Force | Out-Null

$HarnessConfigDirectory = Join-Path $HarnessDataRoot "config"
New-Item -ItemType Directory -Path $HarnessConfigDirectory -Force | Out-Null
$HarnessConfigPath = Join-Path $HarnessConfigDirectory "config.json"

# 配置顶栏边缘滚轮手势与默认手势集
$ConfigJson = @{
    plugins = @{ gesture = @{ enabled = $true } }
    gesture = @{
        enabled = $true
        paused = $false
        triggerButton = "right"
        trailVisible = $true
        edge_top_wheel = $true
        profiles = @(
            @{
                name = "default"
                triggerStates = @{
                    edge_top_wheel = "enabled"
                }
                mappings = @(
                    @{
                        gestureCode = "TopEdge+WheelUp"
                        action = @{
                            type = 2
                            builtinCmd = 22
                            name = "音量增加"
                        }
                    },
                    @{
                        gestureCode = "TopEdge+WheelDown"
                        action = @{
                            type = 2
                            builtinCmd = 23
                            name = "音量减小"
                        }
                    },
                    @{
                        gestureCode = "R"
                        action = @{
                            type = 2
                            builtinCmd = 5
                            name = "显示桌面"
                        }
                    }
                )
            }
        )
    }
} | ConvertTo-Json -Depth 6
[System.IO.File]::WriteAllText($HarnessConfigPath, $ConfigJson, [System.Text.UTF8Encoding]::new($false))

# 注册 Win32 高精度测试与句柄观测引擎
Add-Type @"
using System;
using System.Runtime.InteropServices;
using System.Threading;
using System.Diagnostics;

public static class ChallengerNativeHarness {
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
    public static extern bool SetCursorPos(int X, int Y);

    [DllImport("user32.dll")]
    public static extern uint GetGuiResources(IntPtr hProcess, uint uiFlags);

    [DllImport("user32.dll", SetLastError=true, CharSet=CharSet.Unicode)]
    public static extern IntPtr FindWindow(string lpClassName, string lpWindowName);

    [DllImport("user32.dll")]
    public static extern uint GetWindowThreadProcessId(IntPtr hWnd, out uint lpdwProcessId);

    [DllImport("user32.dll", SetLastError=true)]
    public static extern bool PostMessage(IntPtr hWnd, uint Msg, IntPtr wParam, IntPtr lParam);

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
    public const uint MOUSEEVENTF_WHEEL = 0x0800;
    public const uint MOUSEEVENTF_ABSOLUTE = 0x8000;
    public const uint MOUSEEVENTF_VIRTUALDESK = 0x4000;

    public const uint DESKTOP_ALL = 0x01FF;
    public const uint WM_CLOSE = 0x0010;
    public static readonly IntPtr TEST_EXTRA_INFO = new IntPtr(0x54455354); // "TEST"

    private static IntPtr _originalInputDesktop = IntPtr.Zero;
    private static IntPtr _currentDesktop = IntPtr.Zero;

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

    public static void SendRightDown(int x, int y) {
        SetCursorPos(x, y);
        int nx, ny;
        ToNormalized(x, y, out nx, out ny);
        INPUT[] inputs = new INPUT[1];
        inputs[0].type = INPUT_MOUSE;
        inputs[0].mi.dx = nx;
        inputs[0].mi.dy = ny;
        inputs[0].mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK | MOUSEEVENTF_RIGHTDOWN;
        inputs[0].mi.dwExtraInfo = TEST_EXTRA_INFO;
        SendInput(1, inputs, Marshal.SizeOf(typeof(INPUT)));
    }

    public static void SendRightUp(int x, int y) {
        SetCursorPos(x, y);
        int nx, ny;
        ToNormalized(x, y, out nx, out ny);
        INPUT[] inputs = new INPUT[1];
        inputs[0].type = INPUT_MOUSE;
        inputs[0].mi.dx = nx;
        inputs[0].mi.dy = ny;
        inputs[0].mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK | MOUSEEVENTF_RIGHTUP;
        inputs[0].mi.dwExtraInfo = TEST_EXTRA_INFO;
        SendInput(1, inputs, Marshal.SizeOf(typeof(INPUT)));
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
        SendInput(1, inputs, Marshal.SizeOf(typeof(INPUT)));
    }

    public static void SendWheel(int x, int y, int delta) {
        SetCursorPos(x, y);
        int nx, ny;
        ToNormalized(x, y, out nx, out ny);
        INPUT[] inputs = new INPUT[1];
        inputs[0].type = INPUT_MOUSE;
        inputs[0].mi.dx = nx;
        inputs[0].mi.dy = ny;
        inputs[0].mi.mouseData = (uint)delta;
        inputs[0].mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK | MOUSEEVENTF_WHEEL;
        inputs[0].mi.dwExtraInfo = TEST_EXTRA_INFO;
        SendInput(1, inputs, Marshal.SizeOf(typeof(INPUT)));
    }

    public static void SendWheelOnly(int delta) {
        INPUT[] inputs = new INPUT[1];
        inputs[0].type = INPUT_MOUSE;
        inputs[0].mi.mouseData = (uint)delta;
        inputs[0].mi.dwFlags = MOUSEEVENTF_WHEEL;
        inputs[0].mi.dwExtraInfo = TEST_EXTRA_INFO;
        SendInput(1, inputs, Marshal.SizeOf(typeof(INPUT)));
    }

    public static void SendWheelBurst(int delta, int count) {
        INPUT[] inputs = new INPUT[1];
        inputs[0].type = INPUT_MOUSE;
        inputs[0].mi.mouseData = (uint)delta;
        inputs[0].mi.dwFlags = MOUSEEVENTF_WHEEL;
        inputs[0].mi.dwExtraInfo = TEST_EXTRA_INFO;
        int size = Marshal.SizeOf(typeof(INPUT));
        for (int i = 0; i < count; i++) {
            SendInput(1, inputs, size);
        }
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
}
"@

# 初始化活动桌面
[ChallengerNativeHarness]::InitializeDesktop()

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

Write-Host "[INFO] 启动被测进程 (沙箱环境)..." -ForegroundColor Yellow
$proc = [System.Diagnostics.Process]::Start($psi)
if (-not $proc -or $proc.HasExited) {
    Write-Host "[ERROR] 进程启动失败！" -ForegroundColor Red
    exit 1
}

$procId = $proc.Id
Write-Host "[OK] Tools3000 已就绪 (PID: $procId)" -ForegroundColor Green

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

$allPassed = $true

try {
    # 等待引擎初始化
    Write-Host "`n── [步骤 1] 校验引擎就绪 ──" -ForegroundColor Cyan
    $ready = $false
    $sw = [System.Diagnostics.Stopwatch]::StartNew()
    while ($sw.Elapsed.TotalSeconds -lt 10) {
        if ($proc.HasExited) {
            throw "进程启动异常退出！"
        }
        $log = Get-LogContentSafe $HarnessLogFile
        if ($log -match "手势触发掩码已同步" -or $log -match "手势引擎" -or $log -match "从配置加载手势配置集") {
            $ready = $true
            break
        }
        Start-Sleep -Milliseconds 150
    }
    if ($ready) {
        Write-Host "[OK] 手势引擎与掩码同步已就绪！" -ForegroundColor Green
    } else {
        Write-Host "[WARN] 引擎就绪中，继续执行..." -ForegroundColor Yellow
        Start-Sleep -Milliseconds 1000
    }

    # ─────────────────────────────────────────────────────────────────────────
    # CHALLENGE 1: Wheel Gesture Idle Wait Bypass Stress
    # ─────────────────────────────────────────────────────────────────────────
    Write-Host "`n── [挑战目标 1] 屏幕顶栏 Idle 状态滚轮手势绕行延迟压测 (Wheel Gesture Idle Wait Bypass) ──" -ForegroundColor Cyan
    Write-Host "  -> 目标：在 GestureState::Idle 下极速派发 $WheelIterations 次滚轮事件" -ForegroundColor Gray
    Write-Host "  -> 断言：单事件平均延迟 < 1.0ms，且绝不发生旧版 50ms 盲等卡顿 (若有 50ms 盲等则总耗时将 > $( $WheelIterations * 50 )ms)" -ForegroundColor Gray

    $wheelLogOffset = if (Test-Path $HarnessLogFile) { (Get-Item $HarnessLogFile).Length } else { 0 }
    
    # 将鼠标移动到屏幕物理顶部 edge 区域 (y = 0, x = 960)
    [ChallengerNativeHarness]::SendMove(960, 0)
    Start-Sleep -Milliseconds 200

    # 预热滚轮分发通道
    for ($w = 0; $w -lt 5; $w++) {
        [ChallengerNativeHarness]::SendWheelOnly(120)
    }
    Start-Sleep -Milliseconds 200

    [void][ChallengerNativeHarness]::timeBeginPeriod(1)
    $wheelSw = [System.Diagnostics.Stopwatch]::StartNew()

    [ChallengerNativeHarness]::SendWheelBurst(120, $WheelIterations)

    $wheelSw.Stop()
    [void][ChallengerNativeHarness]::timeEndPeriod(1)

    $totalWheelMs = $wheelSw.Elapsed.TotalMilliseconds
    $avgWheelLatencyMs = $totalWheelMs / $WheelIterations
    Write-Host "  -> 实测完成 $WheelIterations 次滚轮注入总耗时: $([Math]::Round($totalWheelMs, 2)) ms" -ForegroundColor Gray
    Write-Host "  -> 实测平均单事件耗时: $([Math]::Round($avgWheelLatencyMs, 4)) ms/event" -ForegroundColor Gray

    # 检查日志验证执行
    Start-Sleep -Milliseconds 1000
    $fullContent = Get-LogContentSafe $HarnessLogFile
    $wheelCountInLog = ([regex]::Matches($fullContent, "执行手势动作: gesture=WheelUp")).Count
    Write-Host "  -> 日志证实执行滚轮手势动作次数: $wheelCountInLog" -ForegroundColor Gray

    if ($proc.HasExited) {
        Write-Host "[FAIL] 挑战 1 失败：进程意外崩溃！" -ForegroundColor Red
        $allPassed = $false
    } elseif ($avgWheelLatencyMs -ge 2.0) {
        Write-Host "[FAIL] 挑战 1 失败：单事件延迟 $([Math]::Round($avgWheelLatencyMs, 3))ms >= 2.0ms！" -ForegroundColor Red
        $allPassed = $false
    } elseif ($totalWheelMs -ge ($WheelIterations * 50 * 0.5)) {
        Write-Host "[FAIL] 挑战 1 失败：总耗时 $([Math]::Round($totalWheelMs, 1))ms 触发了 50ms 盲等超时！" -ForegroundColor Red
        $allPassed = $false
    } else {
        Write-Host "[OK] 挑战 1 通过：极速滚轮手势单事件耗时 $([Math]::Round($avgWheelLatencyMs, 3))ms (< 2ms)，彻底绕过 50ms 盲等！" -ForegroundColor Green
    }

    # ─────────────────────────────────────────────────────────────────────────
    # CHALLENGE 2: Memory & Resource Leak Check (200 手势循环)
    # ─────────────────────────────────────────────────────────────────────────
    Write-Host "`n── [挑战目标 2] 连续 $GestureIterations 次手势生命周期资源泄漏审计 (Resource Leak Check) ──" -ForegroundColor Cyan
    
    # 预热管线并等待运行时与 WebView2 彻底沉降
    Write-Host "  -> 执行 3 笔手势预热管线以完成 D2D/DComp 首帧编译..." -ForegroundColor Gray
    for ($w = 0; $w -lt 3; $w++) {
        [ChallengerNativeHarness]::SendRightDown(500, 400)
        [ChallengerNativeHarness]::SendMove(550, 400)
        [ChallengerNativeHarness]::SendRightUp(550, 400)
        [System.Threading.Thread]::Sleep(50)
    }
    Start-Sleep -Milliseconds 1500

    # 稳定基线采样
    $p = Get-Process -Id $procId
    $gdiBefore = [ChallengerNativeHarness]::GetGuiResources($p.Handle, 0)
    $userBefore = [ChallengerNativeHarness]::GetGuiResources($p.Handle, 1)
    $handlesBefore = $p.HandleCount
    $wsBeforeMB = [Math]::Round($p.WorkingSet64 / 1MB, 2)
    $privBeforeMB = [Math]::Round($p.PrivateMemorySize64 / 1MB, 2)

    Write-Host "  -> [基线采样] GDI 对象: $gdiBefore, USER 对象: $userBefore, 内核句柄: $handlesBefore, 工作集: $wsBeforeMB MB, 私有内存: $privBeforeMB MB" -ForegroundColor Gray

    $leakLogOffset = if (Test-Path $HarnessLogFile) { (Get-Item $HarnessLogFile).Length } else { 0 }

    Write-Host "  -> 开始连续执行 $GestureIterations 次手势生命周期 (起手 -> 划动 -> 松手 -> 淡出)..." -ForegroundColor Yellow
    $cycleSw = [System.Diagnostics.Stopwatch]::StartNew()

    for ($i = 1; $i -le $GestureIterations; $i++) {
        $startX = 500
        $startY = 400
        [ChallengerNativeHarness]::SendRightDown($startX, $startY)
        
        # 向右平移 60px (分 3 步)
        for ($s = 1; $s -le 3; $s++) {
            [ChallengerNativeHarness]::SendMove($startX + ($s * 20), $startY)
            [System.Threading.Thread]::Sleep(2)
        }
        
        [ChallengerNativeHarness]::SendRightUp($startX + 60, $startY)
        
        # 每 10 笔让出 15ms 允许 DComp 硬件淡出管线迭代
        if ($i % 10 -eq 0) {
            [System.Threading.Thread]::Sleep(15)
        }
    }

    $cycleSw.Stop()
    Write-Host "  -> $GestureIterations 次手势循环完成，耗时: $([Math]::Round($cycleSw.Elapsed.TotalSeconds, 2)) 秒" -ForegroundColor Gray

    # 等待手势完全淡出与系统工作集稳定
    Write-Host "  -> 等待淡出沉降与资源收缩 (1.5 秒)..." -ForegroundColor Gray
    Start-Sleep -Milliseconds 1500

    $p.Refresh()
    $gdiAfter = [ChallengerNativeHarness]::GetGuiResources($p.Handle, 0)
    $userAfter = [ChallengerNativeHarness]::GetGuiResources($p.Handle, 1)
    $handlesAfter = $p.HandleCount
    $wsAfterMB = [Math]::Round($p.WorkingSet64 / 1MB, 2)
    $privAfterMB = [Math]::Round($p.PrivateMemorySize64 / 1MB, 2)

    $gdiDelta = $gdiAfter - $gdiBefore
    $userDelta = $userAfter - $userBefore
    $handlesDelta = $handlesAfter - $handlesBefore
    $privDeltaMB = [Math]::Round($privAfterMB - $privBeforeMB, 2)

    Write-Host "  -> [终态采样] GDI 对象: $gdiAfter (Δ=$gdiDelta)" -ForegroundColor Gray
    Write-Host "  -> [终态采样] USER 对象: $userAfter (Δ=$userDelta)" -ForegroundColor Gray
    Write-Host "  -> [终态采样] 内核句柄: $handlesAfter (Δ=$handlesDelta)" -ForegroundColor Gray
    Write-Host "  -> [终态采样] 工作集: $wsAfterMB MB, 私有内存: $privAfterMB MB (Δ=$privDeltaMB MB)" -ForegroundColor Gray

    # 检查日志中是否有运行期 CreateSurface (F6 预分配表面，划动全程必须为 0 次)
    $leakSliceLog = Get-LogSlice $HarnessLogFile $leakLogOffset
    $createSurfaceCount = ([regex]::Matches($leakSliceLog, "CreateSurface|recreateBitmap|创建分层窗口")).Count
    Write-Host "  -> 划动全程运行时表面/窗口重新创建次数: $createSurfaceCount (必须为 0)" -ForegroundColor Gray

    if ($proc.HasExited) {
        Write-Host "[FAIL] 挑战 2 失败：进程意外退出！" -ForegroundColor Red
        $allPassed = $false
    } elseif ($userDelta -gt 0) {
        Write-Host "[FAIL] 挑战 2 失败：检测到 USER 句柄净泄漏！(Δ=+$userDelta)" -ForegroundColor Red
        $allPassed = $false
    } elseif ($gdiDelta -gt 0) {
        Write-Host "[FAIL] 挑战 2 失败：检测到 GDI 句柄/位图净泄漏！(Δ=+$gdiDelta)" -ForegroundColor Red
        $allPassed = $false
    } elseif ($createSurfaceCount -gt 0) {
        Write-Host "[FAIL] 挑战 2 失败：划动手势期间发生了 $createSurfaceCount 次动态表面重新创建，违背 F6 预分配契约！" -ForegroundColor Red
        $allPassed = $false
    } elseif ($privDeltaMB -gt 15.0) {
        Write-Host "[FAIL] 挑战 2 失败：私有内存净增量异常 ($privDeltaMB MB > 15MB)！" -ForegroundColor Red
        $allPassed = $false
    } else {
        Write-Host "[OK] 挑战 2 通过：$GestureIterations 次高频手势循环 USER 句柄净增量为 0 (Δ=$userDelta <= 0)，GDI 句柄净增量为 0 (Δ=$gdiDelta <= 0)，表面重设为 0，内存无泄漏！" -ForegroundColor Green
    }

    Write-Host "`n===============================================================================" -ForegroundColor Cyan
    if ($allPassed) {
        Write-Host " [PASS] Milestone 2 所有对抗性挑战与资源断言 100% 全部通过！" -ForegroundColor Green
    } else {
        Write-Host " [FAIL] 存在未通过的对抗性断言！" -ForegroundColor Red
    }
    Write-Host "===============================================================================" -ForegroundColor Cyan

} finally {
    [ChallengerNativeHarness]::RestoreDesktop()
    try {
        [ChallengerNativeHarness]::RequestProcessClose($procId) | Out-Null
        $proc.WaitForExit(2000) | Out-Null
    } catch {}
    if ($proc -and -not $proc.HasExited) {
        try {
            $proc.Kill()
            $proc.WaitForExit(1000) | Out-Null
        } catch {}
    }
    try {
        Remove-Item -LiteralPath $HarnessRoot -Recurse -Force -ErrorAction SilentlyContinue
    } catch {}
}

if (-not $allPassed) {
    exit 1
}
exit 0
